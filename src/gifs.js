import { decompressFrames, parseGIF } from 'gifuct-js';
import { MAX_MEDIA_FILE_SIZE } from './images.js';

export const MAX_GIF_FRAMES = 255;
export const MIN_FRAME_DELAY_MS = 20;

export async function decodeGif(file) {
  validateGifFile(file);
  let parsed;
  let frames;
  try {
    parsed = parseGIF(await file.arrayBuffer());
    frames = decompressFrames(parsed, true);
  } catch (error) {
    throw new Error(`GIF decoding failed: ${error.message}`);
  }
  if (frames.length === 0) throw new Error('The GIF does not contain any frames');
  if (frames.length > MAX_GIF_FRAMES) {
    throw new Error(`GIFs are limited to ${MAX_GIF_FRAMES} frames; this file has ${frames.length}`);
  }

  const width = parsed.lsd?.width ?? Math.max(...frames.map((frame) => frame.dims.left + frame.dims.width));
  const height = parsed.lsd?.height ?? Math.max(...frames.map((frame) => frame.dims.top + frame.dims.height));
  return { frames, width, height };
}

export async function* iterateGifFrames(decoded) {
  const canvas = document.createElement('canvas');
  canvas.width = decoded.width;
  canvas.height = decoded.height;
  const context = canvas.getContext('2d');
  const patchCanvas = document.createElement('canvas');
  const patchContext = patchCanvas.getContext('2d');
  if (!context || !patchContext) throw new Error('Canvas GIF processing is unavailable');

  let previousFrame = null;
  let restoreImage = null;
  context.clearRect(0, 0, canvas.width, canvas.height);

  for (let index = 0; index < decoded.frames.length; index += 1) {
    const frame = decoded.frames[index];
    if (previousFrame?.disposalType === 2) {
      const dims = previousFrame.dims;
      context.clearRect(dims.left, dims.top, dims.width, dims.height);
    } else if (previousFrame?.disposalType === 3 && restoreImage) {
      context.putImageData(restoreImage, 0, 0);
    }

    restoreImage = frame.disposalType === 3
      ? context.getImageData(0, 0, canvas.width, canvas.height)
      : null;

    patchCanvas.width = frame.dims.width;
    patchCanvas.height = frame.dims.height;
    patchContext.clearRect(0, 0, patchCanvas.width, patchCanvas.height);
    patchContext.putImageData(
      new ImageData(frame.patch, frame.dims.width, frame.dims.height),
      0,
      0,
    );
    context.drawImage(patchCanvas, frame.dims.left, frame.dims.top);

    yield {
      canvas,
      index,
      delay: Math.max(MIN_FRAME_DELAY_MS, frame.delay || 100),
    };
    previousFrame = frame;
  }
}

export function validateGifFile(file) {
  if (!file) throw new Error('Choose a GIF first');
  if (file.size > MAX_MEDIA_FILE_SIZE) {
    throw new Error('GIF files must be 20 MB or smaller');
  }
  if (file.type !== 'image/gif') throw new Error('The selected file is not a GIF');
}
