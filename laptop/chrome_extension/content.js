(function() {
    console.log('Couch Browser: Content script loaded in ' + (window.self === window.top ? 'top frame' : 'iframe'));

    // Dynamically load site-specific logic (also Main World).
    const domain = window.location.hostname.replace(/^www\./, '');

    
    async function init() {
        const key = `disabled_${domain}`;
        let result = {};

        try {
            result = await chrome.storage.sync.get([
                key,
                'cursorSpeed',
                'scrollSpeed',
                'cursorIdleTimeout'
            ]);
        } catch (error) {
            console.error(
                'GestureControl: Failed to read settings', error
            );
        }

        if (result[key]) {
            console.log(
                `GestureControl: Disabled for ${domain}`
            );
            return;
        }

        try {
            const response = await chrome.runtime.sendMessage({
                type: 'MYO_INJECT'
            });

            if (!response || !response.ok) {
                throw new Error(
                    response?.error || 'Script injection failed'
                );
            }

            postSettings(result);

            console.log(
                'GestureControl: Navigation engine ready'
            );
        } catch (error) {
            console.error(
                'GestureControl: Failed to load navigation',
                error
            );
        }
    }

    function isTransientRelayError(error) {
        const message = error && error.message ? error.message : String(error || '');
        return /extension context invalidated|receiving end does not exist|could not establish connection|message port closed/i.test(message);
    }

    function relayToBackground(data) {
        try {
            // The content script can outlive the extension briefly during an
            // extension reload or tab teardown. In that window Chrome throws
            // synchronously before a message can be delivered.
            if (!chrome.runtime || !chrome.runtime.id) return;

            // Use the callback form so runtime.lastError is consumed. The
            // Promise form otherwise produces an unhandled rejection when the
            // service worker is restarting or the page is being closed.
            chrome.runtime.sendMessage(data, () => {
                const error = chrome.runtime.lastError;
                if (!error || isTransientRelayError(error)) return;
                console.error('Couch Browser: Failed to relay message', error);
            });
        } catch (e) {
            if (!isTransientRelayError(e)) {
                console.error('Couch Browser: Failed to relay message', e);
            }
        }
    }

    // Relay tab-switch intents (LT + shoulder buttons) from the page's
    // Main World to the background service worker, which owns the chrome.tabs API.
    // Only the top frame relays so iframes don't trigger duplicate switches.
    if (window.self === window.top) {
        window.addEventListener('message', (event) => {
            const data = event.data;
            // gamepad.js runs in the page's Main World while this listener runs
            // in the isolated content-script world. The Window identity can be
            // wrapped differently between those worlds, so authenticate using
            // the message marker instead of event.source.
            if (data && data.source === 'couch-browser-extension' && (data.type === 'COUCH_BROWSER_TAB' || data.type === 'COUCH_BROWSER_TAB_CLOSE' || data.type === 'COUCH_BROWSER_TAB_RELOAD')) {
                console.log('Couch Browser: Relaying message to background:', data.type);
                relayToBackground(data);
            }
            if (data && data.source === 'couch-browser-extension' && data.type === 'COUCH_BROWSER_DEFAULT_MODE_SET') {
                chrome.storage.sync.set({ defaultMode: data.mode === 'cursor' ? 'cursor' : 'navigation' });
            }
            if (data && data.source === 'couch-browser-extension' && data.type === 'COUCH_BROWSER_SETTINGS_REQUEST') {
                chrome.storage.sync.get(['cursorSpeed', 'scrollSpeed', 'cursorIdleTimeout']).then(postSettings);
            }
        });
    }

    chrome.storage.onChanged.addListener((changes, area) => {
        if (area !== 'sync') return;
        if (changes.cursorSpeed || changes.scrollSpeed || changes.cursorIdleTimeout) {
            postSettings({
            cursorSpeed: changes.cursorSpeed && changes.cursorSpeed.newValue,
                scrollSpeed: changes.scrollSpeed && changes.scrollSpeed.newValue,
                cursorIdleTimeout: changes.cursorIdleTimeout && changes.cursorIdleTimeout.newValue
            });
        }
    });

    // The popup also sends live updates directly to the active tab. This keeps
    // slider changes responsive even when storage synchronization is delayed.
    chrome.runtime.onMessage.addListener((message) => {
        if (!message || message.type !== 'COUCH_BROWSER_SETTINGS') return;
        postSettings(message);
    });

    function postSettings(settings) {
        const message = {
            source: 'couch-browser-extension',
            type: 'COUCH_BROWSER_SETTINGS'
        };
        if (Number.isFinite(settings.cursorSpeed) && settings.cursorSpeed > 0) message.cursorSpeed = Math.min(settings.cursorSpeed, 50);
        if (Number.isFinite(settings.scrollSpeed) && settings.scrollSpeed > 0) message.scrollSpeed = Math.min(settings.scrollSpeed, 3000);
        if (Number.isFinite(settings.cursorIdleTimeout) && settings.cursorIdleTimeout > 0) {
            message.cursorIdleTimeout = settings.cursorIdleTimeout >= 31
                ? 2147483647
                : Math.min(settings.cursorIdleTimeout, 30) * 1000;
        }
        window.postMessage(message, '*');
    }
    
    init();
})();
