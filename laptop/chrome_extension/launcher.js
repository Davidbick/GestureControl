const grid = document.getElementById('app-grid');

let selected = 0;
const columns = 2;

// Build the launcher tiles.
MYO_APPS.forEach((app) => {
    const tile = document.createElement('a');

    tile.href = app.url;
    tile.className = `tile tile-${app.id}`;
    tile.setAttribute('aria-label', app.name);

    const name = document.createElement('span');
    name.className = 'tile-name';
    name.textContent = app.name;

    const description = document.createElement('span');
    description.className = 'tile-description';
    description.textContent = app.description;

    tile.append(name, description);
    grid.appendChild(tile);
});

const tiles = Array.from(
    grid.querySelectorAll('.tile')
);

function select(index) {
    if (index < 0 || index >= tiles.length) {
        return;
    }

    selected = index;

    tiles.forEach((tile, i) => {
        tile.classList.toggle('selected', i === selected);
    });

    tiles[selected].focus({
        preventScroll: true
    });
}

// D-pad navigation for a two-column grid.
document.addEventListener('keydown', (event) => {
    const key = event.key;

    if (!key.startsWith('Arrow') && key !== 'Enter') {
        return;
    }

    event.preventDefault();

    const row = Math.floor(selected / columns);
    const col = selected % columns;

    let next = selected;

    if (key === 'ArrowLeft' && col > 0) {
        next--;
    } else if (
        key === 'ArrowRight' &&
        col < columns - 1
    ) {
        next++;
    } else if (key === 'ArrowUp' && row > 0) {
        next -= columns;
    } else if (
        key === 'ArrowDown' &&
        next + columns < tiles.length
    ) {
        next += columns;
    } else if (key === 'Enter') {
        tiles[selected].click();
        return;
    }

    select(next);
});

tiles.forEach((tile, index) => {
    tile.addEventListener('focus', () => {
        selected = index;

        tiles.forEach((item, i) => {
            item.classList.toggle(
                'selected', i === index
            );
        });
    });
});

select(0);
