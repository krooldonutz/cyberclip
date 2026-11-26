// Presentation-only behaviour for the Pixie Pixel layout, shared by the
// desktop site and the device-hosted page. Nothing here talks to the board.

export function setupLayoutChrome(doc = document) {
  setupLibraryStrip(doc);
  setupPillPopovers(doc);
  setupDropZone(doc);
}

// The "›" button beside Saved media scrolls the strip one page right, and
// hides itself when there is nothing further to show.
function setupLibraryStrip(doc) {
  const list = doc.getElementById('library-list');
  const next = doc.getElementById('library-next');
  if (!list || !next) return;

  const update = () => {
    next.disabled = list.scrollLeft + list.clientWidth >= list.scrollWidth - 2;
  };
  next.addEventListener('click', () => {
    list.scrollBy({ left: list.clientWidth * 0.8, behavior: 'smooth' });
  });
  list.addEventListener('scroll', update, { passive: true });
  new MutationObserver(update).observe(list, { childList: true });
  globalThis.addEventListener?.('resize', update);
  update();
}

// Quality and Backlight open as small popovers (<details>); close any that
// are open when the person clicks elsewhere or presses Escape.
function setupPillPopovers(doc) {
  const popovers = [...doc.querySelectorAll('details.pill-control')];
  if (popovers.length === 0) return;
  const closeAll = (except) => {
    for (const popover of popovers) {
      if (popover !== except) popover.open = false;
    }
  };
  doc.addEventListener('click', (event) => {
    closeAll(popovers.find((popover) => popover.contains(event.target)));
  });
  doc.addEventListener('keydown', (event) => {
    if (event.key === 'Escape') closeAll();
  });
}

// Dropping a file on the preview hands it to the existing file input, so the
// app's normal change handler processes it.
function setupDropZone(doc) {
  const stage = doc.getElementById('preview-stage');
  const input = doc.getElementById('media-input');
  if (!stage || !input) return;

  const setDragging = (dragging) => stage.classList.toggle('is-dragging', dragging);
  stage.addEventListener('dragover', (event) => {
    event.preventDefault();
    setDragging(true);
  });
  stage.addEventListener('dragleave', () => setDragging(false));
  stage.addEventListener('drop', (event) => {
    event.preventDefault();
    setDragging(false);
    const files = event.dataTransfer?.files;
    if (!files || files.length === 0 || input.disabled) return;
    input.files = files;
    input.dispatchEvent(new Event('change', { bubbles: true }));
  });
}
