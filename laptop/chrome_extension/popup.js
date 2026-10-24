const toggle = document.getElementById('enable-toggle');
const domainText = document.getElementById('domain-name');
const statusText = document.getElementById('status-text');
const versionText = document.getElementById('extension-version');

let currentDomain = '';
let currentTabId = null;

function updateStatus() {
    statusText.textContent = toggle.checked
        ? 'Navigation enabled'
        : 'Navigation disabled';
}

async function init() {
    versionText.textContent =
        chrome.runtime.getManifest().version;

    const [tab] = await chrome.tabs.query({
        active: true,
        currentWindow: true
    });

    // Chrome internal pages cannot use this extension.
    if (!tab || !tab.url ||
        !/^https?:\/\//.test(tab.url)) {

        statusText.textContent = 'Unavailable';
        domainText.textContent = 'Open a website';
        return;
    }

    currentTabId = tab.id;

    const url = new URL(tab.url);
    currentDomain = url.hostname.replace(/^www\./, '');

    domainText.textContent = currentDomain;

    // Reuse the site setting from content.js.
    const key = `disabled_${currentDomain}`;

    const settings = await chrome.storage.sync.get(key);

    toggle.checked = !settings[key];
    toggle.disabled = false;

    updateStatus();

    toggle.addEventListener('change', async () => {
        toggle.disabled = true;

        const previous = !toggle.checked;

        try {
            if (toggle.checked) {
                await chrome.storage.sync.remove(key);
            } else {
                await chrome.storage.sync.set({
                    [key]: true
                });
            }

            updateStatus();

            // Apply the change to the current webpage.
            if (currentTabId != null) {
                await chrome.tabs.reload(currentTabId);
            }
        } catch (err) {
            toggle.checked = previous;
            updateStatus();
            console.error('Site toggle failed:', err);
        } finally {
            toggle.disabled = false;
        }
    });
}

init().catch((err) => {
    statusText.textContent = 'Extension error';
    console.error('Popup initialization failed:', err);
});
