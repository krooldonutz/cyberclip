import { describe, expect, it } from 'vitest';
import { MAX_GIF_FRAMES, validateGifFile } from './gifs.js';

describe('GIF validation', () => {
  it('rejects non-GIF files', () => {
    expect(() => validateGifFile({ size: 100, type: 'image/png' })).toThrow('not a GIF');
  });

  it('rejects files over 20 MB', () => {
    expect(() => validateGifFile({
      size: 20 * 1024 * 1024 + 1,
      type: 'image/gif',
    })).toThrow('20 MB');
  });

  it('keeps the protocol frame limit explicit', () => {
    expect(MAX_GIF_FRAMES).toBe(255);
  });
});
