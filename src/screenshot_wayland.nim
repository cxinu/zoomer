## Wayland screenshot capture using grim.
## Captures screen to PPM format via stdout, parses into ImageData.

import osproc
import streams
import strutils
import image_data

proc captureScreen*(): ImageData =
  ## Runs `grim -t ppm -` to capture the entire screen as PPM to stdout,
  ## then parses the PPM P6 data into an ImageData (BGRA format).
  let process = startProcess("grim", args = @["-t", "ppm", "-"],
                              options = {poUsePath, poStdErrToStdOut})
  let output = process.outputStream.readAll()
  let exitCode = process.waitForExit()
  process.close()

  if exitCode != 0:
    quit "grim failed (exit code " & $exitCode & "): " & output

  # Parse PPM P6 format
  var pos = 0

  # Read magic "P6\n"
  if output.len < 3 or output[0] != 'P' or output[1] != '6':
    quit "grim output is not PPM P6 format"
  pos = 2
  while pos < output.len and output[pos] in {'\n', '\r', ' '}: inc pos

  # Skip comments
  while pos < output.len and output[pos] == '#':
    while pos < output.len and output[pos] != '\n': inc pos
    inc pos

  # Read width
  var widthStr = ""
  while pos < output.len and output[pos] in {'0'..'9'}:
    widthStr.add(output[pos])
    inc pos
  while pos < output.len and output[pos] in {' ', '\t'}: inc pos

  # Read height
  var heightStr = ""
  while pos < output.len and output[pos] in {'0'..'9'}:
    heightStr.add(output[pos])
    inc pos
  while pos < output.len and output[pos] in {'\n', '\r', ' '}: inc pos

  # Skip comments
  while pos < output.len and output[pos] == '#':
    while pos < output.len and output[pos] != '\n': inc pos
    inc pos

  # Read maxval
  var maxvalStr = ""
  while pos < output.len and output[pos] in {'0'..'9'}:
    maxvalStr.add(output[pos])
    inc pos
  # Skip single whitespace after maxval
  inc pos

  let width = parseInt(widthStr).cint
  let height = parseInt(heightStr).cint
  let pixelCount = width * height

  # Allocate BGRA buffer
  let dataSize = pixelCount * 4
  result.width = width
  result.height = height
  result.bpp = 4
  result.ownsData = true
  result.data = cast[cstring](alloc(dataSize))

  # Convert RGB (PPM) to BGRA
  for i in 0..<pixelCount:
    let srcIdx = pos + i * 3
    let dstIdx = i * 4
    if srcIdx + 2 < output.len:
      result.data[dstIdx + 0] = output[srcIdx + 2]  # B
      result.data[dstIdx + 1] = output[srcIdx + 1]  # G
      result.data[dstIdx + 2] = output[srcIdx + 0]  # R
      result.data[dstIdx + 3] = chr(255)             # A
