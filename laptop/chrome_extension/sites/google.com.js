(function () {
    if (!window.CouchBrowser) {
        return;
    }

    window.CouchBrowser.registerSite({
        name: 'google.com',
        indicatorColor: '#4CAF50',
        nesting: 'innermost',

        extraSelectors: [
            'textarea[name="q"]',
            'input[name="q"]',
            '[role="listbox"] [role="option"]',
            '.sbct',
            'button[type="submit"]',
            'input[type="submit"]'
        ],

        overlaySelectors: [
            '[role="listbox"]',
            '[role="dialog"]',
            '[aria-modal="true"]'
        ],

        firstElementSelectors: [
            'textarea[name="q"]',
            'input[name="q"]'
        ],

        getContainer: function (el) {
            return el.closest('[role="option"]')
                || el.closest('.sbct')
                || el;
        }
    });
})();
