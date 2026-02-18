## Backend-agnostic image data type.
## Replaces PXImage for use in both X11 and Wayland backends.

type ImageData* = object
  width*: cint
  height*: cint
  data*: cstring     ## BGRA pixel data (4 bytes per pixel)
  bpp*: cint         ## bytes per pixel (always 4)
  ownsData*: bool    ## if true, data was allocated by us and must be freed

proc destroy*(img: var ImageData) =
  if img.ownsData and img.data != nil:
    dealloc(img.data)
    img.data = nil
