## Wayland backend for boomer.
## This file is compiled when boomer.nim is built with -d:wayland.

import os
import navigation
import config
import wayland_ffi
import screenshot_wayland
import image_data
import opengl
import la
import strutils
import math
import options

# Linux input-event-codes.h key constants
const
  KEY_ESC*   = 1
  KEY_0*     = 11
  KEY_MINUS* = 12
  KEY_EQUAL* = 13
  KEY_Q*     = 16
  KEY_R*     = 19
  KEY_F*     = 33

# --- Shader loading (same as X11 backend) ---
type Shader = tuple[path, content: string]

proc readShader(file: string): Shader =
  when nimvm:
    result.path = file
    result.content = slurp result.path
  else:
    result.path = "src" / file
    result.content = readFile result.path

const
  vertexShader = readShader "vert.glsl"
  fragmentShader = readShader "frag.glsl"

proc newShader(shader: Shader, kind: GLenum): GLuint =
  result = glCreateShader(kind)
  var shaderArray = allocCStringArray([shader.content])
  glShaderSource(result, 1, shaderArray, nil)
  glCompileShader(result)
  deallocCStringArray(shaderArray)

  var success: GLint
  var infoLog = newString(512).cstring
  glGetShaderiv(result, GL_COMPILE_STATUS, addr success)
  if not success.bool:
    glGetShaderInfoLog(result, 512, nil, infoLog)
    echo "------------------------------"
    echo "Error during shader compilation: ", shader.path, ". Log:"
    echo infoLog
    echo "------------------------------"

proc newShaderProgram(vertex, fragment: Shader): GLuint =
  result = glCreateProgram()

  var
    vertexShader = newShader(vertex, GL_VERTEX_SHADER)
    fragmentShader = newShader(fragment, GL_FRAGMENT_SHADER)

  glAttachShader(result, vertexShader)
  glAttachShader(result, fragmentShader)

  glLinkProgram(result)

  glDeleteShader(vertexShader)
  glDeleteShader(fragmentShader)

  var success: GLint
  var infoLog = newString(512).cstring
  glGetProgramiv(result, GL_LINK_STATUS, addr success)
  if not success.bool:
    glGetProgramInfoLog(result, 512, nil, infoLog)
    echo infoLog

  glUseProgram(result)

# --- Flashlight ---
type Flashlight = object
  isEnabled: bool
  shadow: float32
  radius: float32
  deltaRadius: float32

const
  INITIAL_FL_DELTA_RADIUS = 250.0
  FL_DELTA_RADIUS_DECELERATION = 10.0

proc update(flashlight: var Flashlight, dt: float32) =
  if abs(flashlight.deltaRadius) > 1.0:
    flashlight.radius = max(0.0, flashlight.radius + flashlight.deltaRadius * dt)
    flashlight.deltaRadius -= flashlight.deltaRadius * FL_DELTA_RADIUS_DECELERATION * dt

  if flashlight.isEnabled:
    flashlight.shadow = min(flashlight.shadow + 6.0 * dt, 0.8)
  else:
    flashlight.shadow = max(flashlight.shadow - 6.0 * dt, 0.0)

proc mainWayland() =
  let boomerDir = getConfigDir() / "boomer"
  var configFile = boomerDir / "config"
  var windowed = false
  var delaySec = 0.0

  block:
    proc versionQuit() =
      const hash = gorgeEx("git rev-parse HEAD")
      quit "boomer-$#" % [if hash.exitCode == 0: hash.output[0 .. 7] else: "unknown"]
    proc usageQuit() =
      quit """Usage: boomer [OPTIONS]
  -d, --delay <seconds: float>  delay execution of the program by provided <seconds>
  -h, --help                    show this help and exit
      --new-config [filepath]   generate a new default config at [filepath]
  -c, --config <filepath>       use config at <filepath>
  -V, --version                 show the current version and exit
  -w, --windowed                windowed mode instead of fullscreen"""
    var i = 1
    while i <= paramCount():
      let arg = paramStr(i)

      template asParam(paramVar: untyped, body: untyped) =
        if i + 1 > paramCount():
          echo "No value is provided for $#" % [arg]
          usageQuit()
        let paramVar = paramStr(i + 1)
        body
        i += 2

      template asFlag(body: untyped) =
        body
        i += 1

      case arg
      of "-d", "--delay":
        asParam(delayParam):
          delaySec = parseFloat(delayParam)
      of "-w", "--windowed":
        asFlag():
          windowed = true
      of "-h", "--help":
        asFlag():
          usageQuit()
      of "-V", "--version":
        asFlag():
          versionQuit()
      of "--new-config":
        var configName = none(string)
        if i + 1 <= paramCount():
          let param = paramStr(i + 1)
          if len(param) > 0 and param[0] != '-':
            configName = some(param)

        let newConfigPath = configName.get(configFile)

        createDir(newConfigPath.splitFile.dir)
        if newConfigPath.fileExists:
          stdout.write("File ", newConfigPath, " already exists. Replace it? [yn] ")
          if stdin.readChar != 'y':
            quit "Disaster prevented"

        generateDefaultConfig(newConfigPath)
        quit "Generated config at $#" % [newConfigPath]
      of "-c", "--config":
        asParam(configParam):
          configFile = configParam
      else:
        echo "Unknown flag `$#`" % [arg]
        usageQuit()
  sleep(floor(delaySec * 1000).int)

  var config = defaultConfig

  if fileExists configFile:
    config = loadConfig(configFile)
  else:
    stderr.writeLine configFile & " doesn't exist. Using default values. "

  echo "Using config: ", config

  # Take screenshot before creating the window
  echo "Capturing screenshot via grim..."
  var screenshot = captureScreen()
  defer: screenshot.destroy()

  # Initialize Wayland backend
  var wlState = wl_backend_init(if windowed: 1.cint else: 0.cint)
  if cast[pointer](wlState) == nil:
    quit "Failed to initialize Wayland backend"
  defer: wl_backend_destroy(wlState)

  # Load OpenGL extensions (EGL context is already current from init)
  loadExtensions()

  var shaderProgram = newShaderProgram(vertexShader, fragmentShader)

  let w = screenshot.width.float32
  let h = screenshot.height.float32
  var
    vao, vbo, ebo: GLuint
    vertices = [
      # Position                 Texture coords
      [GLfloat    w,     0, 0.0, 1.0, 1.0], # Top right
      [GLfloat    w,     h, 0.0, 1.0, 0.0], # Bottom right
      [GLfloat    0,     h, 0.0, 0.0, 0.0], # Bottom left
      [GLfloat    0,     0, 0.0, 0.0, 1.0]  # Top left
    ]
    indices = [GLuint(0), 1, 3,
                      1,  2, 3]

  glGenVertexArrays(1, addr vao)
  glGenBuffers(1, addr vbo)
  glGenBuffers(1, addr ebo)
  defer:
    glDeleteVertexArrays(1, addr vao)
    glDeleteBuffers(1, addr vbo)
    glDeleteBuffers(1, addr ebo)

  glBindVertexArray(vao)

  glBindBuffer(GL_ARRAY_BUFFER, vbo)
  glBufferData(GL_ARRAY_BUFFER, size = GLsizeiptr(sizeof(vertices)),
               addr vertices, GL_STATIC_DRAW)

  glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, ebo);
  glBufferData(GL_ELEMENT_ARRAY_BUFFER, size = GLsizeiptr(sizeof(indices)),
               addr indices, GL_STATIC_DRAW);

  var stride = GLsizei(vertices[0].len * sizeof(GLfloat))

  glVertexAttribPointer(0, 3, cGL_FLOAT, false, stride, cast[pointer](0))
  glEnableVertexAttribArray(0)

  glVertexAttribPointer(1, 2, cGL_FLOAT, false, stride, cast[pointer](3 * sizeof(GLfloat)))
  glEnableVertexAttribArray(1)

  var texture = 0.GLuint
  glGenTextures(1, addr texture)
  glActiveTexture(GL_TEXTURE0)
  glBindTexture(GL_TEXTURE_2D, texture)

  glTexImage2D(GL_TEXTURE_2D,
               0,
               GL_RGB.GLint,
               screenshot.width,
               screenshot.height,
               0,
               GL_BGRA,
               GL_UNSIGNED_BYTE,
               screenshot.data)
  glGenerateMipmap(GL_TEXTURE_2D)

  glUniform1i(glGetUniformLocation(shaderProgram, "tex".cstring), 0)

  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST)
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST)
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_BORDER)
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_BORDER)

  let rate = wl_state_output_rate(wlState)
  let dt = 1.0 / rate.float

  # Wait until compositor sends fullscreen configure
  while wl_state_configured(wlState) == 0:
    discard wl_backend_roundtrip(wlState)

  # Render the first frame immediately so the window appears with
  # screenshot content (the window becomes visible on first eglSwapBuffers)
  block:
    let w = wl_state_width(wlState)
    let h = wl_state_height(wlState)
    glViewport(0, 0, w, h)
    glClearColor(0.1, 0.1, 0.1, 1.0)
    glClear(GL_COLOR_BUFFER_BIT or GL_DEPTH_BUFFER_BIT)
    glUseProgram(shaderProgram)
    glUniform2f(glGetUniformLocation(shaderProgram, "cameraPos".cstring), 0.0, 0.0)
    glUniform1f(glGetUniformLocation(shaderProgram, "cameraScale".cstring), 1.0)
    glUniform2f(glGetUniformLocation(shaderProgram, "screenshotSize".cstring),
                screenshot.width.float32, screenshot.height.float32)
    glUniform2f(glGetUniformLocation(shaderProgram, "windowSize".cstring), w.float32, h.float32)
    glUniform2f(glGetUniformLocation(shaderProgram, "cursorPos".cstring), 0.0, 0.0)
    glUniform1f(glGetUniformLocation(shaderProgram, "flShadow".cstring), 0.0)
    glUniform1f(glGetUniformLocation(shaderProgram, "flRadius".cstring), 200.0)
    glBindVertexArray(vao)
    glDrawElements(GL_TRIANGLES, count = 6, GL_UNSIGNED_INT, indices = nil)
    wl_backend_swap_buffers(wlState)

  var
    quitting = false
    camera = Camera(scale: 1.0)
    mouse: Mouse =
      block:
        let pos = vec2(wl_state_pointer_x(wlState).float32,
                        wl_state_pointer_y(wlState).float32)
        Mouse(curr: pos, prev: pos)
    flashlight = Flashlight(
      isEnabled: false,
      radius: 200.0)

  proc scrollUp() =
    if wl_state_ctrl_held(wlState) != 0 and flashlight.isEnabled:
      flashlight.deltaRadius += INITIAL_FL_DELTA_RADIUS
    else:
      camera.deltaScale += config.scrollSpeed
      camera.scalePivot = mouse.curr

  proc scrollDown() =
    if wl_state_ctrl_held(wlState) != 0 and flashlight.isEnabled:
      flashlight.deltaRadius -= INITIAL_FL_DELTA_RADIUS
    else:
      camera.deltaScale -= config.scrollSpeed
      camera.scalePivot = mouse.curr

  while not quitting:
    # Poll and dispatch Wayland events (non-blocking)
    discard wl_backend_poll_events(wlState)

    # Check close
    if wl_state_closed(wlState) != 0:
      quitting = true
      break

    let winWidth  = wl_state_width(wlState)
    let winHeight = wl_state_height(wlState)

    glViewport(0, 0, winWidth, winHeight)

    # Update mouse position
    mouse.curr = vec2(wl_state_pointer_x(wlState).float32,
                       wl_state_pointer_y(wlState).float32)

    # Handle mouse button
    if wl_state_button_just_pressed(wlState) != 0:
      mouse.prev = mouse.curr
      mouse.drag = true
      camera.velocity = vec2(0.0, 0.0)

    if wl_state_button_just_released(wlState) != 0:
      mouse.drag = false

    # Handle mouse drag
    if mouse.drag:
      let delta = world(camera, mouse.prev) - world(camera, mouse.curr)
      camera.position += delta
      camera.velocity = delta * rate.float

    mouse.prev = mouse.curr

    # Handle scroll
    let scrollDelta = wl_state_scroll_delta(wlState)
    if scrollDelta > 0:
      for i in 0..<scrollDelta:
        scrollUp()
    elif scrollDelta < 0:
      for i in 0..<(-scrollDelta):
        scrollDown()

    # Handle keyboard events (iterate the queue)
    let keyCount = wl_state_key_event_count(wlState)
    for i in 0.cint..<keyCount:
      let keyCode = wl_state_key_event_key(wlState, i)
      let keyState = wl_state_key_event_state(wlState, i)
      if keyState == 1:  # key press
        case keyCode
        of KEY_EQUAL: scrollUp()
        of KEY_MINUS: scrollDown()
        of KEY_0:
          camera.scale = 1.0
          camera.deltaScale = 0.0
          camera.position = vec2(0.0'f32, 0.0)
          camera.velocity = vec2(0.0'f32, 0.0)
        of KEY_Q, KEY_ESC:
          quitting = true
        of KEY_R:
          if configFile.len > 0 and fileExists(configFile):
            config = loadConfig(configFile)
        of KEY_F:
          flashlight.isEnabled = not flashlight.isEnabled
        else:
          discard

    # Reset per-frame input state AFTER processing
    wl_state_reset_frame(wlState)

    camera.update(config, dt, mouse, windowSize = vec2(winWidth.float32, winHeight.float32))
    flashlight.update(dt)

    # Draw
    glClearColor(0.1, 0.1, 0.1, 1.0)
    glClear(GL_COLOR_BUFFER_BIT or GL_DEPTH_BUFFER_BIT)

    glUseProgram(shaderProgram)

    glUniform2f(glGetUniformLocation(shaderProgram, "cameraPos".cstring), camera.position[0], camera.position[1])
    glUniform1f(glGetUniformLocation(shaderProgram, "cameraScale".cstring), camera.scale)
    glUniform2f(glGetUniformLocation(shaderProgram, "screenshotSize".cstring),
                screenshot.width.float32,
                screenshot.height.float32)
    glUniform2f(glGetUniformLocation(shaderProgram, "windowSize".cstring),
                winWidth.float32,
                winHeight.float32)
    glUniform2f(glGetUniformLocation(shaderProgram, "cursorPos".cstring),
                mouse.curr.x.float32,
                mouse.curr.y.float32)
    glUniform1f(glGetUniformLocation(shaderProgram, "flShadow".cstring), flashlight.shadow)
    glUniform1f(glGetUniformLocation(shaderProgram, "flRadius".cstring), flashlight.radius)

    glBindVertexArray(vao)
    glDrawElements(GL_TRIANGLES, count = 6, GL_UNSIGNED_INT, indices = nil)

    wl_backend_swap_buffers(wlState)

mainWayland()

