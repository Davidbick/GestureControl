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
        panel.classList.add('myo-open');

        panel.querySelector(
            '[data-myo-action="home"]'
        ).focus();
    }

    function hideMenu() {
        panel.classList.remove('myo-open');
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
