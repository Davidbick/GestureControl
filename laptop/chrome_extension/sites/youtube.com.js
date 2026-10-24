(function () {
    if (!window.CouchBrowser) {
        return;
    }

    window.CouchBrowser.registerSite({
        name: 'youtube.com',
        indicatorColor: '#FF3333',
        nesting: 'innermost',

        extraSelectors: [
            // Search
            'input#search',
            'yt-searchbox input',
            'ytd-searchbox-suggestions [role="option"]',
            'yt-searchbox-suggestions-section [role="option"]',

            // Video cards
            'ytd-rich-item-renderer a#thumbnail',
            'ytd-video-renderer a#thumbnail',
            'ytd-compact-video-renderer a#thumbnail',
            'a#video-title-link',

            // Navigation and menus
            'ytd-guide-entry-renderer a',
            'ytd-mini-guide-entry-renderer a',
            'ytd-menu-service-item-renderer',
            '[role="menuitem"]',

            // Player controls
            '.ytp-play-button',
            '.ytp-next-button',
            '.ytp-fullscreen-button',
            '.ytp-settings-button'
        ],

        overlaySelectors: [
            'ytd-searchbox-suggestions',
            'yt-searchbox-suggestions-section',
            'tp-yt-iron-dropdown[opened]',
            '[role="dialog"]',
            '[aria-modal="true"]'
        ],

        closeSelectors: [
            'button[aria-label*="close" i]',
            'button[aria-label*="dismiss" i]',
            '[aria-label*="Close" i]'
        ],

        firstElementSelectors: [
            'input#search',
            'yt-searchbox input',
            'ytd-rich-item-renderer a#thumbnail',
            'ytd-video-renderer a#thumbnail'
        ],

        getContainer: function (el) {
            return el.closest('ytd-rich-item-renderer')
                || el.closest('ytd-video-renderer')
                || el.closest('ytd-compact-video-renderer')
                || el;
        }
    });
})();
