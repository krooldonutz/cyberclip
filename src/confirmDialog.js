// An in-page replacement for the browser's confirm pop-up. Cancel is the default button:
// it has focus when the dialog opens, and Escape or a click outside the
// dialog also cancels.
//
// Resolves true only when the confirm button is pressed. The confirm click
// counts as a user gesture, so callers can still open browser pickers
// (navigator.serial.requestPort) right after awaiting it.
export function confirmAction({
  title,
  message,
  confirmLabel = 'Continue',
  cancelLabel = 'Cancel',
  destructive = false,
  document: doc = globalThis.document,
}) {
  const dialog = doc.createElement('dialog');
  dialog.className = 'confirm-dialog';
  dialog.setAttribute('aria-labelledby', 'confirm-dialog-title');
  dialog.setAttribute('aria-describedby', 'confirm-dialog-message');

  const heading = doc.createElement('h2');
  heading.id = 'confirm-dialog-title';
  heading.textContent = title;

  const body = doc.createElement('p');
  body.id = 'confirm-dialog-message';
  body.textContent = message;

  const cancelButton = doc.createElement('button');
  cancelButton.type = 'button';
  cancelButton.className = 'button button-secondary';
  cancelButton.dataset.action = 'cancel';
  cancelButton.textContent = cancelLabel;
  cancelButton.autofocus = true;

  const confirmButton = doc.createElement('button');
  confirmButton.type = 'button';
  confirmButton.className = `button ${destructive ? 'button-destructive' : 'button-primary'}`;
  confirmButton.dataset.action = 'confirm';
  confirmButton.textContent = confirmLabel;

  const actions = doc.createElement('div');
  actions.className = 'confirm-dialog-actions';
  actions.append(cancelButton, confirmButton);
  // The dialog itself has no padding, so a click whose target is the dialog
  // element is a click on the backdrop.
  const content = doc.createElement('div');
  content.className = 'confirm-dialog-content';
  content.append(heading, body, actions);
  dialog.append(content);
  doc.body.append(dialog);

  return new Promise((resolve) => {
    let settled = false;
    const finish = (confirmed) => {
      if (settled) return;
      settled = true;
      if (typeof dialog.close === 'function' && dialog.open) dialog.close();
      dialog.remove();
      resolve(confirmed);
    };

    cancelButton.addEventListener('click', () => finish(false));
    confirmButton.addEventListener('click', () => finish(true));
    // Escape fires "cancel" on a modal dialog.
    dialog.addEventListener('cancel', (event) => {
      event.preventDefault();
      finish(false);
    });
    dialog.addEventListener('keydown', (event) => {
      if (event.key === 'Escape') finish(false);
    });
    // A click on the backdrop lands on the dialog element itself.
    dialog.addEventListener('click', (event) => {
      if (event.target === dialog) finish(false);
    });

    if (typeof dialog.showModal === 'function') {
      try {
        dialog.showModal();
      } catch {
        dialog.setAttribute('open', '');
      }
    } else {
      dialog.setAttribute('open', '');
    }
    cancelButton.focus();
  });
}
