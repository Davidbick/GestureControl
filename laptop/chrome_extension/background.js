// Couch Browser background service worker.
//
// Tab switching cannot be done from a content script or the page's Main World —
// it requires the chrome.tabs API, which is only available in extension
// contexts. content.js relays COUCH_BROWSER_TAB intents (triggered by LT +
// shoulder buttons) here, and we activate the adjacent tab.

// Cooldown so a single button press never triggers multiple actions (tab switches
// or closes). A press can produce several messages (multiple frames relaying,
// or controller button bounce); we ignore any that arrive within this window
// of the last action.
importScripts('apps.js');
const INPUT_COOLDOWN_MS = 200;
let lastInputTime = 0;

chrome.runtime.onMessage.addListener((msg, sender, sendResponse) => {
    if (!msg) return;
    
    // Inject the navigation engine through Chrome
    // rather than through webpage script elements.
    if (msg.type === 'MYO_INJECT') {
        if (!sender.tab ||
            typeof sender.tab.id !== 'number' ||
            typeof sender.frameId !== 'number') {
            sendResponse({
                ok: false,
                error: 'Missing target tab or frame'
            });
            return;
        }

        let domain = '';

        try {
            domain = new URL(sender.url)
                .hostname.replace(/^www\./, '');
        } catch (e) {
            sendResponse({
                ok: false,
                error: 'Invalid page URL'
            });
            return;
        }

        const sites = {
            'youtube.com': 'sites/youtube.com.js',
            'google.com': 'sites/google.com.js',
            'netflix.com': 'sites/netflix.com.js'
        };

        const files = [
            'virtual-keyboard.js',
            'core.js',
            'hid-input.js',
            sites[domain] || 'sites/default.js'
        ];

        chrome.scripting.executeScript({
            target: {
                tabId: sender.tab.id,
                frameIds: [sender.frameId]
            },
            files: files,
            world: 'MAIN'
        }).then(() => {
            sendResponse({ ok: true });
        }).catch((error) => {
            console.error('GestureControl injection failed', error);
            sendResponse({
                ok: false,
                error: error.message
            });
        });

        // Keep the message channel open until injection finishes.
        return true;
    }

    // GestureControl TV navigation.
    // Only respond to messages from a webpage
    // containing our extension's content script.
    if (msg.type === 'MYO_HOME' ||
        msg.type === 'MYO_APP') {

        if (!sender.tab ||
            typeof sender.tab.id !== 'number') {
            return;
        }

        let url = null;

        if (msg.type === 'MYO_HOME') {
            url = chrome.runtime.getURL('launcher.html');
        } else {
            const app = MYO_APPS.find(
                (item) => item.id === msg.appId
            );

            if (app) {
                url = app.url;
            }
        }

        if (url) {
            chrome.tabs.update(
                sender.tab.id,
                { url }
            ).catch((error) => {
                console.error(
                    'GestureControl navigation failed',
                    error
                );
            });
        }

        return;
    }
    const now = Date.now();
    console.log('Last input time and now:', lastInputTime, now);
    if (now - lastInputTime < INPUT_COOLDOWN_MS) return;
    lastInputTime = now;

    switch (msg.type) {
        case 'COUCH_BROWSER_TAB_CLOSE':
            console.log('Couch Browser: Received COUCH_BROWSER_TAB_CLOSE');
            const removeCallback = () => {
                if (chrome.runtime.lastError) {
                    console.error('Couch Browser: Failed to remove tab:', chrome.runtime.lastError.message);
                } else {
                    console.log('Couch Browser: Tab removed successfully');
                }
            };

            if (sender.tab && typeof sender.tab.id === 'number') {
                console.log('Couch Browser: Removing tab', sender.tab.id);
                chrome.tabs.remove(sender.tab.id, removeCallback);
            } else {
                console.log('Couch Browser: sender.tab not available, querying active tab');
                // Fallback for when sender.tab is not available
                chrome.tabs.query({ active: true, currentWindow: true }, (tabs) => {
                    if (tabs && tabs[0]) {
                        console.log('Couch Browser: Removing active tab', tabs[0].id);
                        chrome.tabs.remove(tabs[0].id, removeCallback);
                    } else {
                        console.error('Couch Browser: No active tab found to close');
                    }
                });
            }
            break;
        case 'COUCH_BROWSER_TAB_RELOAD':
            console.log('Couch Browser: Received COUCH_BROWSER_TAB_RELOAD');
            if (sender.tab && typeof sender.tab.id === 'number') {
                reloadTab(sender.tab.id);
            } else {
                // Normally sender.tab is present for a content-script message,
                // but use the active tab as a safe fallback if Chrome omits it.
                chrome.tabs.query({ active: true, currentWindow: true }, (tabs) => {
                    if (tabs && tabs[0] && typeof tabs[0].id === 'number') {
                        reloadTab(tabs[0].id);
                    } else {
                        console.error('Couch Browser: No current tab found to reload');
                    }
                });
            }
            break;
        case 'COUCH_BROWSER_TAB':
            console.log('Couch Browser: Received COUCH_BROWSER_TAB');
            const dir = msg.dir === 'next' ? 1 : -1;

            chrome.tabs.query({ currentWindow: true }, (tabs) => {
                if (!tabs || tabs.length === 0) return;

                // Order by tab strip position.
                const sorted = tabs.slice().sort((a, b) => a.index - b.index);
                const activeIdx = sorted.findIndex(t => t.active);
                if (activeIdx === -1) return;

                // Move to the adjacent tab, wrapping around at the ends.
                let next = (activeIdx + dir) % sorted.length;
                if (next < 0) next += sorted.length;

                const target = sorted[next];
                if (target && typeof target.id === 'number') {
                    chrome.tabs.update(target.id, { active: true });
                }
            });
            break;
    }
});

function reloadTab(tabId) {
    console.log('Couch Browser: Reloading tab', tabId);
    chrome.tabs.reload(tabId, {}, () => {
        if (chrome.runtime.lastError) {
            console.error('Couch Browser: Failed to reload tab:', chrome.runtime.lastError.message);
        }
    });
}
