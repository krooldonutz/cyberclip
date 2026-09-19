export const MAX_MEDIA_FILE_SIZE = 20 * 1024 * 1024;
const MIN_JPEG_QUALITY = 0.35;
const QUALITY_STEP = 0.08;

export async function prepareImageFile(file, options) {
  validateImageFile(file);
  const bitmap = await createImageBitmap(file);
  try {
    return await encodeSourceForDisplay(bitmap, options);
  } finally {
    bitmap.close();
  }
}

export async function encodeSourceForDisplay(source, {
  width,
  height,
  rotation = 0,
  fit = 'contain',
  background = '#000000',
  quality = 0.82,
  maxFrameSize = 128 * 1024,
}) {
  const dimensions = getRotatedDimensions(width, height, rotation);
  const canvas = createCanvas(dimensions.width, dimensions.height);
  const context = canvas.getContext('2d', { alpha: false });
  if (!context) throw new Error('Canvas image processing is unavailable');

  context.fillStyle = background;
  context.fillRect(0, 0, canvas.width, canvas.height);
  drawFitted(context, source, canvas.width, canvas.height, fit);

  let currentQuality = clamp(quality, MIN_JPEG_QUALITY, 0.95);
  let blob;
  do {
    blob = await canvasToBlob(canvas, 'image/jpeg', currentQuality);
    if (blob.size <= maxFrameSize) break;
    currentQuality = Math.max(MIN_JPEG_QUALITY, currentQuality - QUALITY_STEP);
  } while (currentQuality > MIN_JPEG_QUALITY);

  if (!blob || blob.size > maxFrameSize) {
    throw new Error(
      `The processed JPEG is ${(blob?.size ?? 0).toLocaleString()} bytes, above the device limit of ${maxFrameSize.toLocaleString()} bytes`,
    );
  }

  return {
    bytes: new Uint8Array(await blob.arrayBuffer()),
    blob,
    canvas,
    width: canvas.width,
    height: canvas.height,
    quality: currentQuality,
  };
}

export function getRotatedDimensions(width, height, rotation) {
  return rotation % 2 === 0
    ? { width, height }
    : { width: height, height: width };
}

export function validateImageFile(file) {
  if (!file) throw new Error('Choose an image first');
  if (file.size > MAX_MEDIA_FILE_SIZE) {
    throw new Error('Media files must be 20 MB or smaller');
  }
  if (!['image/jpeg', 'image/png', 'image/webp'].includes(file.type)) {
    throw new Error('Choose a JPEG, PNG, or WebP image');
  }
}

function drawFitted(context, source, targetWidth, targetHeight, fit) {
  const sourceWidth = source.width;
  const sourceHeight = source.height;
  if (!sourceWidth || !sourceHeight) throw new Error('The image has invalid dimensions');

  if (fit === 'stretch') {
    context.drawImage(source, 0, 0, targetWidth, targetHeight);
    return;
  }

  const scale = fit === 'cover'
    ? Math.max(targetWidth / sourceWidth, targetHeight / sourceHeight)
    : Math.min(targetWidth / sourceWidth, targetHeight / sourceHeight);
  const width = sourceWidth * scale;
  const height = sourceHeight * scale;
  context.drawImage(
    source,
    (targetWidth - width) / 2,
    (targetHeight - height) / 2,
    width,
    height,
  );
}

function createCanvas(width, height) {
  if (typeof OffscreenCanvas !== 'undefined') return new OffscreenCanvas(width, height);
  const canvas = document.createElement('canvas');
  canvas.width = width;
  canvas.height = height;
  return canvas;
}

function canvasToBlob(canvas, type, quality) {
  if ('convertToBlob' in canvas) return canvas.convertToBlob({ type, quality });
  return new Promise((resolve, reject) => {
    canvas.toBlob(
      (blob) => blob ? resolve(blob) : reject(new Error('JPEG encoding failed')),
      type,
      quality,
    );
  });
}

function clamp(value, min, max) {
  return Math.min(max, Math.max(min, value));
}
