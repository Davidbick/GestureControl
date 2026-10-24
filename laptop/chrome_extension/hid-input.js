
(function () {
    // Avoid registering input handlers twice.
    if (window.GestureControlHidInput) {
        return;
    }

    window.GestureControlHidInput = true;

    const SOURCE = 'couch-browser-extension';

    const navigationKeys = new Set([
        'ArrowUp',
        'ArrowDown',
        'ArrowLeft',
        'ArrowRight',
        'Enter',
        'Escape'
    ]);

    const REPEAT_INTERVAL_MS = 120;
    let lastRepeatMs = 0;

    // Send an action to the Couch Browser engine.
    function send(type, data = {}) {
        window.postMessage({
            source: SOURCE,
            type: type,
            ...data
        }, '*');
    }

    function keyboardOpen() {
        return window.CouchBrowserVirtualKeyboard &&
            window.CouchBrowserVirtualKeyboard.isOpen();
    }

    function handleKeyDown(event) {
        // Only intercept real keyboard/HID input.
        if (!event.isTrusted ||
            event.isComposing ||
            event.ctrlKey ||
            event.altKey ||
            event.metaKey) {
            return;
        }

        let action = event.key;

        // The virtual keyboard temporarily owns input.
        if (keyboardOpen()) {
            if (action === 'Enter') {
                action = 'KeyboardActivate';
            } else if (action === 'Backspace') {
                action = 'KeyboardBackspace';
            }
        }

        if (!navigationKeys.has(action) &&
            action !== 'KeyboardActivate' &&
            action !== 'KeyboardBackspace') {
            return;
        }

        // Stop the website from also interpreting
        // this key as a normal arrow or Enter press.
        event.preventDefault();
        event.stopImmediatePropagation();

        // Allow held arrows, but limit repeat speed.
        if (event.repeat) {
            const now = performance.now();

            if (now - lastRepeatMs < REPEAT_INTERVAL_MS) {
                return;
            }

            lastRepeatMs = now;
        }

        send('COUCH_BROWSER_KEY', {
            key: action
        });
    }

    function handleKeyUp(event) {
        if (!event.isTrusted ||
            event.ctrlKey ||
            event.altKey ||
            event.metaKey) {
            return;
        }

        if (navigationKeys.has(event.key) ||
            (event.key === 'Backspace' && keyboardOpen())) {
            event.preventDefault();
            event.stopImmediatePropagation();
        }
    }

    window.addEventListener(
        'keydown', handleKeyDown, true);

    window.addEventListener(
        'keyup', handleKeyUp, true);

    // Force spatial navigation instead of cursor mode.
    window.CouchBrowserDefaultMode = 'navigation';

    send('COUCH_BROWSER_DEFAULT_MODE', {
        mode: 'navigation'
    });

    // Tell the navigation engine that input is active.
    send('COUCH_BROWSER_CONNECTION', {
        connected: true
    });

    console.log('GestureControl HID navigation ready');
})();
