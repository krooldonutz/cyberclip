// Registers the offline cache in production builds and removes any left over
// from a production build during development.
export function registerServiceWorker(log) {
  if (import.meta.env.PROD && 'serviceWorker' in navigator && globalThis.isSecureContext) {
    navigator.serviceWorker.register('/service-worker.js').catch((error) => {
      log(`Offline support could not start: ${error.message}`, 'error');
    });
  } else if ('serviceWorker' in navigator) {
    void navigator.serviceWorker.getRegistrations()
      .then((registrations) => Promise.all(
        registrations.map((registration) => registration.unregister()),
      ));
  }
}
