import { bridgeAvailable, call, onEvent } from './rpc.js';
import { store } from './store.js';
import { gameStore } from './game-store.js';
import { downloadStore } from './download-store.js';
import { startRouter } from './router.js';
import { setStatus } from './ui.js';
import { el, clear, formatError } from './utils.js';

const MAXIMIZE_ICON =
    '<svg viewBox="0 0 10 10" aria-hidden="true"><rect x="2.5" y="2.5" width="5" height="5" rx=".6"/></svg>';
const RESTORE_ICON =
    '<svg viewBox="0 0 10 10" aria-hidden="true">' +
    '<rect x="3.4" y="1.7" width="4.9" height="4.9" rx=".6"/>' +
    '<rect x="1.7" y="3.4" width="4.9" height="4.9" rx=".6"/>' +
    '</svg>';

function initWindowChrome() {
    const titlebar = document.getElementById('titlebar');
    const maxButton = document.getElementById('window-max');
    if (!titlebar || !maxButton) return;

    const applyState = (maximized) => {
        document.body.classList.toggle('window-maximized', maximized);
        maxButton.innerHTML = maximized ? RESTORE_ICON : MAXIMIZE_ICON;
        maxButton.title = maximized ? '还原' : '最大化';
        maxButton.setAttribute('aria-label', maxButton.title);
    };

    document.getElementById('window-min')?.addEventListener('click', () => {
        call('window.minimize').catch(() => {});
    });
    maxButton.addEventListener('click', async () => {
        try {
            const result = await call('window.toggle_maximize');
            applyState(Boolean(result?.maximized));
        } catch (_) {
            // 窗口命令失败时保持当前状态。
        }
    });
    document.getElementById('window-close')?.addEventListener('click', () => {
        call('window.close').catch(() => {});
    });

    onEvent('window.state', (data) => applyState(Boolean(data?.maximized)));
    call('window.state')
        .then((result) => applyState(Boolean(result?.maximized)))
        .catch(() => {});

    const isInteractive = (target) =>
        Boolean(target?.closest?.(
            'button, input, a, .breadcrumb, .top-actions, .window-controls'));

    // 移动超过 4px 才进入系统拖动，避免影响单击/双击。
    titlebar.addEventListener('mousedown', (event) => {
        if (event.button !== 0 || isInteractive(event.target)) return;
        const startX = event.clientX;
        const startY = event.clientY;
        let dragging = false;

        const stop = () => {
            window.removeEventListener('mousemove', move, true);
            window.removeEventListener('mouseup', stop, true);
        };
        const move = (moveEvent) => {
            if (dragging) return;
            if (Math.hypot(moveEvent.clientX - startX,
                           moveEvent.clientY - startY) < 4) return;
            dragging = true;
            stop();
            call('window.drag').catch(() => {});
        };

        window.addEventListener('mousemove', move, true);
        window.addEventListener('mouseup', stop, true);
    });

    titlebar.addEventListener('dblclick', async (event) => {
        if (isInteractive(event.target)) return;
        try {
            const result = await call('window.toggle_maximize');
            applyState(Boolean(result?.maximized));
        } catch (_) {
            // 忽略。
        }
    });

    for (const edge of ['top', 'bottom', 'left', 'right',
                        'top-left', 'top-right', 'bottom-left', 'bottom-right']) {
        const handle = el('div', { className: `window-resize ${edge}` });
        handle.addEventListener('mousedown', (event) => {
            if (event.button !== 0) return;
            event.preventDefault();
            call('window.resize', { edge }).catch(() => {});
        });
        document.body.append(handle);
    }
}

function fatal(error) {
    const page = document.getElementById('page');
    if (page) {
        clear(page);
        const card = el('div', { className: 'card' });
        card.append(
            el('h2', { text: '启动器界面无法初始化', style: 'margin:0 0 10px' }),
            el('div', { className: 'alert', text: formatError(error) }),
            el('p', {
                className: 'muted',
                style: 'margin:12px 0 0;font-size:12.5px;line-height:1.7',
                text: '请确认通过 cmcl-ui.exe 启动，并且 WebView2 Runtime 已安装。',
            }),
        );
        page.append(card);
    }
    setStatus('界面初始化失败');
}

function initFileDrop() {
    const host = window.chrome?.webview;
    if (!host?.postMessageWithAdditionalObjects) return;

    const overlay = el('div', { className: 'drop-overlay hidden' });
    const card = el('div', { className: 'drop-card' });
    card.append(
        el('div', { className: 'drop-icon', text: '包' }),
        el('h3', { text: '松开以导入整合包' }),
        el('p', {
            text: '支持 Modrinth .mrpack，以及包含 modrinth.index.json 的 .zip',
        }),
    );
    overlay.append(card);
    document.body.append(overlay);

    let depth = 0;
    const show = () => overlay.classList.remove('hidden');
    const hide = () => {
        depth = 0;
        overlay.classList.add('hidden');
    };

    window.addEventListener('dragenter', (event) => {
        event.preventDefault();
        depth += 1;
        show();
    });
    window.addEventListener('dragover', (event) => {
        event.preventDefault();
        if (event.dataTransfer) event.dataTransfer.dropEffect = 'copy';
    });
    window.addEventListener('dragleave', (event) => {
        event.preventDefault();
        depth -= 1;
        if (depth <= 0) hide();
    });
    window.addEventListener('drop', (event) => {
        event.preventDefault();
        hide();
        const files = [...(event.dataTransfer?.files || [])]
            .filter((file) => /\.(mrpack|zip)$/i.test(file.name));
        if (!files.length) {
            setStatus('只支持 .mrpack / .zip 整合包');
            return;
        }
        host.postMessageWithAdditionalObjects({ type: 'files.dropped' }, files);
    });
}

async function importDroppedFiles(paths) {
    if (!Array.isArray(paths) || !paths.length) return;
    const packs = paths.filter((path) => /\.(mrpack|zip)$/i.test(path));
    if (!packs.length) {
        setStatus('只支持 .mrpack / .zip 整合包');
        return;
    }
    if (downloadStore.busy) {
        setStatus('已有下载任务正在进行');
        return;
    }

    let last = null;
    for (const pack of packs) {
        const name = pack.split(/[\\/]/).pop() || pack;
        setStatus('正在导入 ' + name + '…');
        try {
            last = await downloadStore.start('modpack', {
                mc_dir: store.mcDir,
                path: pack,
            });
            try {
                await store.refreshInstances();
            } catch (_) {
                // 实例详情页会自行刷新。
            }
        } catch (error) {
            setStatus('导入失败：' + formatError(error).split('\n')[0]);
            return;
        }
    }

    if (last?.instance_id) {
        location.hash = '#/instances/' + encodeURIComponent(last.instance_id);
    }
}

async function boot() {
    if (!bridgeAvailable()) {
        fatal({
            message: 'WebView2 桥不可用',
            context: '请通过 cmcl-ui.exe 启动本界面。',
        });
        return;
    }

    window.addEventListener('error', (event) => {
        console.error('[web]', event.message);
        setStatus('脚本错误：' + (event.message || '未知错误'));
    });
    window.addEventListener('unhandledrejection', (event) => {
        console.error('[web]', event.reason);
        setStatus('错误：' + formatError(event.reason).split('\n')[0]);
    });

    initWindowChrome();

    try {
        await store.init();
    } catch (error) {
        fatal(error);
        return;
    }

    gameStore.init();
    downloadStore.init();
    initFileDrop();

    onEvent('files.dropped', (data) => {
        importDroppedFiles(data?.paths);
    });

    const globalDownload = document.getElementById('global-download');
    downloadStore.subscribe((state) => {
        if (!globalDownload) return;
        if (state.busy) {
            globalDownload.classList.remove('hidden');
            globalDownload.textContent =
                `下载 ${Math.round(state.overallPercent())}%`;
        } else {
            globalDownload.classList.add('hidden');
        }
    });

    startRouter();
}

if (document.readyState === 'loading') {
    document.addEventListener('DOMContentLoaded', boot, { once: true });
} else {
    boot();
}
