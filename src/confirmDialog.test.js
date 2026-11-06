import { afterEach, describe, expect, it } from 'vitest';
import { confirmAction } from './confirmDialog.js';

const openDialog = () => document.querySelector('dialog.confirm-dialog');
const button = (action) => openDialog().querySelector(`[data-action="${action}"]`);

describe('confirmAction', () => {
  afterEach(() => {
    document.body.innerHTML = '';
  });

  it('shows the title and message and resolves true on confirm', async () => {
    const result = confirmAction({
      title: 'Install firmware 2.1.0?',
      message: 'Keep the USB cable connected.',
      confirmLabel: 'Install',
    });

    expect(openDialog().hasAttribute('open')).toBe(true);
    expect(openDialog().querySelector('h2').textContent).toBe('Install firmware 2.1.0?');
    expect(openDialog().querySelector('p').textContent).toBe('Keep the USB cable connected.');
    expect(button('confirm').textContent).toBe('Install');
    expect(button('confirm').classList.contains('button-primary')).toBe(true);

    button('confirm').click();
    await expect(result).resolves.toBe(true);
    expect(openDialog()).toBeNull();
  });

  it('focuses Cancel by default and resolves false on cancel', async () => {
    const result = confirmAction({ title: 'Delete?', message: 'Gone for good.', destructive: true });

    expect(document.activeElement).toBe(button('cancel'));
    expect(button('confirm').classList.contains('button-destructive')).toBe(true);

    button('cancel').click();
    await expect(result).resolves.toBe(false);
    expect(openDialog()).toBeNull();
  });

  it('cancels on Escape and on a backdrop click', async () => {
    const escaped = confirmAction({ title: 'A', message: 'a' });
    openDialog().dispatchEvent(new KeyboardEvent('keydown', { key: 'Escape', bubbles: true }));
    await expect(escaped).resolves.toBe(false);

    const backdrop = confirmAction({ title: 'B', message: 'b' });
    openDialog().querySelector('p').click();
    expect(openDialog()).not.toBeNull();
    openDialog().click();
    await expect(backdrop).resolves.toBe(false);
  });

  it('does not render the message as HTML', async () => {
    const result = confirmAction({ title: '<b>x</b>', message: '<img src=x>' });
    expect(openDialog().querySelector('img')).toBeNull();
    button('cancel').click();
    await result;
  });
});
