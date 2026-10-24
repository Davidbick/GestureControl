(async function () {
    // Only show one menu, never one per iframe.
    if (window !== window.top) {
        return;
    }

    // Respect the existing per-site enable switch.
    const domain = location.hostname.replace(/^www\./, '');
    const key = `disabled_${domain}`;
    const settings = await chrome.storage.sync.get(key);

    if (settings[key]) {
        return;
    }

    if (document.readyState === 'loading') {
        await new Promise((resolve) => {
            document.addEventListener(
                'DOMContentLoaded', resolve, { once: true }
            );
        });
    }

    const style = document.createElement('style');

    style.textContent = `
        #myo-tv-open {
            position: fixed !important;
            left: 18px !important;
            bottom: 18px !important;
            z-index: 2147483500 !important;
            padding: 10px 18px !important;
            background: #171a20 !important;
            color: #fff !important;
            border: 2px solid #87e5a4 !important;
            border-radius: 24px !important;
            font: bold 13px Arial, sans-serif !important;
            cursor: pointer !important;
        }

        #myo-tv-panel {
            position: fixed !important;
            inset: 0 !important;
            z-index: 2147483500 !important;
            display: none;
            align-items: center !important;
            justify-content: center !important;
            background: #000b !important;
            font-family: Arial, sans-serif !important;
        }

        #myo-tv-panel .myo-menu {
            width: min(900px, 90vw) !important;
            max-height: 90vh !important;
            overflow-y: auto !important;
            padding: 32px !important;
            border-radius: 22px !important;
            background: #171a20 !important;
            color: #fff !important;
            box-sizing: border-box !important;
        }

        #myo-tv-panel h2 {
            color: white !important;
            font: bold 28px Arial, sans-serif !important;
            margin: 0 0 24px !important;
        }

        #myo-tv-panel .myo-actions,
        #myo-tv-panel .myo-apps {
            display: grid !important;
            grid-template-columns: repeat(2, 1fr) !important;
            gap: 12px !important;
            margin-bottom: 18px !important;
        }

        #myo-tv-panel button {
            min-height: 70px !important;
            padding: 16px !important;
            background: #30343e !important;
            border: 3px solid transparent !important;
            border-radius: 12px !important;
            color: white !important;
            font: bold 17px Arial, sans-serif !important;
            cursor: pointer !important;
        }

        #myo-tv-panel button:hover,
        #myo-tv-panel button:focus-visible {
            outline: none !important;
            border-color: white !important;
            background: #414855 !important;
        }
    `;

    document.documentElement.appendChild(style);

    const openButton = document.createElement('button');
    openButton.id = 'myo-tv-open';
    openButton.type = 'button';
    openButton.textContent = 'MYO';
    openButton.setAttribute(
        'aria-label', 'Open GestureControl menu'
    );

    const panel = document.createElement('section');
    panel.id = 'myo-tv-panel';
    panel.setAttribute('role', 'dialog');
    panel.setAttribute('aria-modal', 'true');
    panel.setAttribute(
        'aria-label', 'GestureControl menu'
    );

    panel.innerHTML = `
        <div class="myo-menu">
            <h2>GestureControl</h2>

            <div class="myo-actions">
                <button type="button"
                        data-myo-action="home">
                    Home
                </button>

                <button type="button"
                        data-myo-action="back">
                    Back
                </button>

                <button type="button"
                        data-myo-action="play">
                    Play / Pause
                </button>

                <button type="button"
                        data-myo-action="close"
                        aria-label="Close GestureControl menu">
                    Close
                </button>
            </div>

            <div class="myo-apps"></div>
        </div>
    `;

    // The menu uses the same app list as the launcher.
    const appGrid = panel.querySelector('.myo-apps');

    MYO_APPS.forEach((app) => {
        const button = document.createElement('button');

        button.type = 'button';
        button.textContent = app.name;
        button.dataset.myoApp = app.id;

        appGrid.appendChild(button);
    });

    function showMenu() {
        panel.style.display = 'flex';

        panel.querySelector(
            '[data-myo-action="home"]'
        ).focus();
    }

    function hideMenu() {
        panel.style.display = 'none';
        openButton.focus();
    }

    function navigate(type, appId = null) {
        chrome.runtime.sendMessage({
            type,
            appId
        }).catch((error) => {
            console.error(
                'GestureControl navigation failed', error
            );
        });
    }

    async function togglePlayback() {
        // Prefer the website's own player control.
        const playerButton = document.querySelector(
            '.ytp-play-button, ' +
            '[data-uia="player-play-pause"]'
        );

        if (playerButton) {
            playerButton.click();
            hideMenu();
            return;
        }

        // Fallback for ordinary HTML5 video players.
        const video = document.querySelector('video');

        if (!video) {
            console.log('GestureControl: No video found');
            return;
        }

        try {
            if (video.paused) {
                await video.play();
            } else {
                video.pause();
            }

            hideMenu();
        } catch (error) {
            console.error(
                'GestureControl playback failed', error
            );
        }
    }

    openButton.addEventListener('click', showMenu);

    panel.addEventListener('click', (event) => {
        if (event.target === panel) {
            hideMenu();
            return;
        }

        const button = event.target.closest('button');

        if (!button) {
            return;
        }

        if (button.dataset.myoApp) {
            navigate('MYO_APP', button.dataset.myoApp);
            return;
        }

        const action = button.dataset.myoAction;

        if (action === 'home') {
            navigate('MYO_HOME');
        } else if (action === 'back') {
            hideMenu();
            window.history.back();
        } else if (action === 'play') {
            togglePlayback();
        } else if (action === 'close') {
            hideMenu();
        }
    });

    document.body.append(openButton, panel);
})();
