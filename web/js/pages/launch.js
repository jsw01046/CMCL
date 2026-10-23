import { call } from '../rpc.js';
import { store } from '../store.js';
import { gameStore } from '../game-store.js';
import {
    el,
    clear,
    badgeClass,
    typeLabel,
    loaderLabel,
    loaderBadgeClass,
    classifyLine,
    formatError,
} from '../utils.js';
import { setStatus, setBreadcrumb, setTopActions, setSidebar, setSidebarFooter } from '../ui.js';
import { createInstanceList, createInstanceSearch } from '../components/instance-list.js';
import { createLogView } from '../components/log-view.js';

let ctx = null;
let list = null;
let log = null;
let unsubscribeLines = null;
let unsubscribeState = null;
let selectedId = null;
let inspectError = false;
let inspectSeq = 0;

let detailTitle = null;
let detailBadge = null;
let detailLoaderBadge = null;
let detailEmpty = null;
let detailList = null;
let detailError = null;
let launchButton = null;
let stopButton = null;
let inspectButton = null;
let savesButton = null;
let pathInput = null;

const rows = {};

function detailRow(label) {
    const row = el('div');
    row.append(el('dt', { text: label }));
    const value = el('dd');
    row.append(value);
    detailList.append(row);
    return value;
}

function buildDetailCard() {
    const card = el('div', { className: 'card' });
    const head = el('div', { className: 'card-head' });
    detailTitle = el('h2', { text: '选择一个版本' });
    detailLoaderBadge = el('span', { className: 'badge loader hidden' });
    detailBadge = el('span', { className: 'badge' });
    head.append(detailTitle, el('div', { className: 'badge-group' }, [
        detailLoaderBadge,
        detailBadge,
    ]));

    detailEmpty = el('div', {
        className: 'empty',
        text: '在左侧列表中选择一个 Minecraft 版本。',
    });
    detailList = el('dl', { className: 'detail hidden' });
    detailError = el('div', { className: 'alert hidden' });

    card.append(head, detailEmpty, detailList, detailError);

    rows.vanilla = detailRow('原版版本');
    rows.loader = detailRow('加载器');
    rows.main = detailRow('主类');
    rows.folder = detailRow('实例文件夹');
    rows.gameDir = detailRow('游戏目录');
    rows.java = detailRow('Java');
    rows.classpath = detailRow('Classpath');
    rows.command = detailRow('命令行');
    rows.natives = detailRow('Natives');

    return card;
}

function buildSettingsCard() {
    const card = el('div', { className: 'card' });
    const head = el('div', { className: 'card-head' });
    head.append(el('h2', { text: '启动设置' }));

    const grid = el('div', { className: 'form-grid' });
    const settings = store.settings;

    const username = el('input', {
        type: 'text',
        value: settings.username,
        maxlength: '16',
        spellcheck: 'false',
    });
    const maxMem = el('input', { type: 'number', value: settings.maxMem, min: '512', step: '256' });
    const width = el('input', { type: 'number', value: settings.width, min: '320', step: '1' });
    const height = el('input', { type: 'number', value: settings.height, min: '240', step: '1' });
    const server = el('input', {
        type: 'text',
        value: settings.server,
        placeholder: '留空则直接进入单人界面',
        spellcheck: 'false',
    });
    const fullscreen = el('input', { type: 'checkbox', checked: settings.fullscreen });

    const field = (label, input, span = false) => {
        const node = el('label', { className: span ? 'span-2' : '' });
        node.append(el('span', { text: label }), input);
        return node;
    };

    grid.append(
        field('玩家名', username),
        field('最大内存 (MB)', maxMem),
        field('窗口宽度', width),
        field('窗口高度', height),
        field('自动进入服务器', server, true),
        el('label', { className: 'check span-2' }, [fullscreen, el('span', { text: '全屏启动' })]),
    );

    const save = () => {
        store.settings.username = username.value.trim() || 'Player';
        store.settings.maxMem = Number(maxMem.value) || 4096;
        store.settings.width = Number(width.value) || 854;
        store.settings.height = Number(height.value) || 480;
        store.settings.server = server.value.trim();
        store.settings.fullscreen = fullscreen.checked;
        store.save();
        setStatus('启动设置已保存');
    };

    for (const node of [username, maxMem, width, height, server, fullscreen]) {
        node.addEventListener('change', save);
    }

    const accountHint = el('div', { className: 'account-hint muted' });
    const link = (text) => el('a', { href: '#/settings/account', text });
    call('auth.status').then((result) => {
        if (!ctx) return;
        clear(accountHint);
        if (result.logged_in && result.account) {
            accountHint.append(
                el('span', {
                    text: `已登录 Microsoft 账号：${result.account.name}，启动时将使用正版身份。`,
                }),
                link('管理账号'),
            );
        } else {
            accountHint.append(
                el('span', { text: '当前未登录 Microsoft 账号，将使用离线玩家名启动。' }),
                link('去登录'),
            );
        }
    }).catch(() => {
        // 账号信息读取失败不影响启动设置显示。
    });

    card.append(head, grid, accountHint);
    return card;
}

function buildToolbar() {
    pathInput = el('input', {
        className: 'path-input',
        type: 'text',
        value: store.mcDir,
        spellcheck: 'false',
    });
    const apply = el('button', {
        className: 'btn ghost small',
        text: '应用',
        type: 'button',
        onclick: applyMcDir,
    });
    pathInput.addEventListener('keydown', (event) => {
        if (event.key === 'Enter') applyMcDir();
    });

    const wrap = el('div', { className: 'path-wrap' });
    wrap.append(pathInput);

    return el('div', { className: 'toolbar' }, [
        el('span', { className: 'muted', text: '.minecraft' }),
        wrap,
        apply,
    ]);
}

function applyMcDir() {
    const value = pathInput.value.trim();
    if (!value) return;
    store.setMcDir(value);
    refreshInstances();
}

async function refreshInstances() {
    setStatus('正在扫描版本…');
    try {
        const instances = await store.refreshInstances();
        pathInput.value = store.mcDir;
        list.setData(instances, selectedId);

        if (!instances.length) {
            clearDetail();
            setStatus('没有找到任何版本');
            return;
        }

        const preferred = instances.find((item) => item.id === selectedId);
        const first = preferred ||
            instances.find((item) => item.valid) ||
            instances[0];
        await selectInstance(first.id);
    } catch (error) {
        setStatus('扫描失败：' + formatError(error).split('\n')[0]);
        detailError.textContent = formatError(error);
        detailError.classList.remove('hidden');
    }
}

function clearDetail() {
    inspectError = false;
    detailTitle.textContent = '选择一个版本';
    detailBadge.textContent = '';
    detailBadge.className = 'badge';
    detailLoaderBadge.textContent = '';
    detailLoaderBadge.className = 'badge loader hidden';
    detailList.classList.add('hidden');
    detailEmpty.classList.remove('hidden');
    detailError.classList.add('hidden');
    renderGameState(gameStore);
}

async function selectInstance(id) {
    if (!id) return;
    selectedId = id;
    store.selected = id;
    store.save();
    list.setSelected(id);

    inspectError = false;
    detailTitle.textContent = id;
    detailBadge.textContent = '';
    detailBadge.className = 'badge';
    detailLoaderBadge.textContent = '';
    detailLoaderBadge.className = 'badge loader hidden';
    detailList.classList.add('hidden');
    detailEmpty.classList.remove('hidden');
    detailEmpty.textContent = '正在检查版本参数…';
    detailError.classList.add('hidden');
    renderGameState(gameStore);

    const token = ++inspectSeq;
    setStatus(`正在检查 ${id}…`);
    try {
        const info = await call('instance.inspect', store.launchParams({ version: id }));
        if (token !== inspectSeq) return;
        renderDetail(info);
    } catch (error) {
        if (token !== inspectSeq) return;
        inspectError = true;
        detailEmpty.classList.add('hidden');
        detailList.classList.add('hidden');
        detailError.textContent = formatError(error);
        detailError.classList.remove('hidden');
        setStatus('检查失败');
        renderGameState(gameStore);
    }
}

function renderDetail(info) {
    detailTitle.textContent = info.id || selectedId || '版本';
    detailBadge.textContent = typeLabel(info.type);
    detailBadge.className = 'badge ' + badgeClass(info.type);

    const loader = info.loader || store.instance(info.id)?.loader || 'vanilla';
    detailLoaderBadge.textContent = loaderLabel(loader);
    detailLoaderBadge.className = loaderBadgeClass(loader);
    rows.loader.textContent = loaderLabel(loader);

    const java = info.java;
    rows.vanilla.textContent = info.vanilla_id || '—';
    rows.main.textContent = info.main_class || '—';
    rows.folder.textContent = info.folder || '—';
    rows.folder.title = info.folder || '';
    rows.gameDir.textContent = info.game_dir
        ? info.game_dir +
          (info.isolate
              ? '（完整隔离）'
              : (info.isolate_saves === false ? '（共享）' : '（存档隔离）'))
        : '—';
    rows.gameDir.title = info.game_dir || '';
    rows.java.textContent = java
        ? `Java ${java.version}（${java.bits} 位）` +
          (info.required_java ? ` · 要求 Java ${info.required_java}+` : '')
        : '自动检测';
    rows.java.title = java ? java.path : '';
    rows.classpath.textContent = info.classpath_count != null
        ? `${info.classpath_count} 项`
        : '—';
    rows.command.textContent = info.command_length != null
        ? `${info.command_length} 字符`
        : '—';
    rows.natives.textContent = info.natives_dir || '—';
    rows.natives.title = info.natives_dir || '';

    detailEmpty.classList.add('hidden');
    detailList.classList.remove('hidden');

    if (info.error) {
        inspectError = true;
        detailError.textContent = formatError(info.error);
        detailError.classList.remove('hidden');
        setStatus('参数检查未通过');
    } else {
        inspectError = false;
        detailError.classList.add('hidden');
        setStatus('就绪');
    }
    renderGameState(gameStore);
}

function renderGameState(state) {
    if (launchButton) {
        launchButton.disabled = state.launching || state.running ||
            !selectedId || inspectError;
        launchButton.textContent = state.launching ? '启动中…' : '启动游戏';
    }
    if (stopButton) {
        stopButton.classList.toggle('hidden', !state.running);
    }
    if (inspectButton) {
        inspectButton.disabled = !selectedId || state.launching;
    }
    if (savesButton) {
        savesButton.disabled = !selectedId;
    }
    if (!log) return;

    if (state.launching) {
        log.setState('启动中…');
    } else if (state.running) {
        log.setState('运行中');
    } else if (state.exitCode !== null && state.exitCode !== undefined) {
        log.setState(state.manualStop ? '已停止' : `已退出 (${state.exitCode})`);
    } else if (!log.summaryShown) {
        log.setState('未启动');
    }
    if (state.summary) log.showSummary(state.summary);
}

async function launch() {
    if (!selectedId || inspectError || gameStore.launching || gameStore.running) return;

    log.reset();
    gameStore.begin(selectedId);
    log.append(`[launcher] 正在启动 ${selectedId} …`, 'sys');
    setStatus('正在启动…');

    try {
        // 不在这里传 isolate，让宿主读取最新的实例设置，避免检查后设置变化。
        const result = await call('game.launch', store.launchParams({
            version: selectedId,
        }));
        gameStore.started(result.pid);
        log.append(`[launcher] 已启动，PID ${result.pid}`, 'sys');
        setStatus('游戏运行中');
    } catch (error) {
        gameStore.fail();
        log.append('[launcher] 启动失败：' + formatError(error), 'err');
        log.setState('启动失败');
        setStatus('启动失败');
    }
}

async function openSaves() {
    if (!selectedId) return;
    try {
        await call('instance.open_saves', {
            mc_dir: store.mcDir,
            id: selectedId,
        });
        setStatus('已打开存档文件夹');
    } catch (error) {
        setStatus('打开存档文件夹失败：' + formatError(error).split('\n')[0]);
    }
}

async function stop() {
    try {
        await call('game.stop');
        log.append('[launcher] 已发送停止请求', 'sys');
    } catch (error) {
        log.append('[launcher] 停止失败：' + formatError(error), 'err');
    }
}

async function listJava() {
    setStatus('正在检测 Java…');
    log.append('[launcher] 正在检测 Java…', 'sys');
    try {
        const result = await call('java.list', { mc_dir: store.mcDir });
        const javas = result.javas || [];
        log.append(`[launcher] 检测到 ${javas.length} 个 Java`, 'sys');
        for (const java of javas) {
            log.append(`  Java ${java.version}（${java.major}，${java.bits} 位） ${java.path}`);
        }
        log.setState('Java 检测完成');
        setStatus(`检测到 ${javas.length} 个 Java`);
    } catch (error) {
        log.append('[launcher] Java 检测失败：' + formatError(error), 'err');
        setStatus('Java 检测失败');
    }
}

export function mount(context) {
    ctx = context;
    selectedId = store.selected;
    inspectSeq++;

    setBreadcrumb('启动');
    setStatus('正在扫描版本…');

    inspectButton = el('button', {
        className: 'btn ghost',
        text: '检查参数',
        type: 'button',
        onclick: () => selectInstance(selectedId),
    });
    savesButton = el('button', {
        className: 'btn ghost',
        text: '存档文件夹',
        type: 'button',
        onclick: openSaves,
    });
    launchButton = el('button', {
        className: 'btn primary',
        text: '启动游戏',
        type: 'button',
        onclick: launch,
    });
    stopButton = el('button', {
        className: 'btn danger hidden',
        text: '停止',
        type: 'button',
        onclick: stop,
    });
    setTopActions([inspectButton, savesButton, launchButton, stopButton]);

    list = createInstanceList({
        onSelect: (id) => selectInstance(id),
    });
    setSidebar([createInstanceSearch(list), list.element]);
    setSidebarFooter([
        el('button', {
            className: 'btn ghost small',
            text: '刷新',
            type: 'button',
            onclick: refreshInstances,
        }),
        el('button', {
            className: 'btn ghost small',
            text: '检测 Java',
            type: 'button',
            onclick: listJava,
        }),
    ]);

    const detailCard = buildDetailCard();
    const settingsCard = buildSettingsCard();
    log = createLogView();

    const grid = el('div', { className: 'detail-grid' }, [detailCard, settingsCard]);
    ctx.page.append(buildToolbar(), grid, log.element);

    for (const line of gameStore.lines) {
        log.append(line, classifyLine(line));
    }
    if (gameStore.summary) log.showSummary(gameStore.summary);

    unsubscribeLines = gameStore.subscribeLines((line) => {
        log.append(line, classifyLine(line));
    });
    unsubscribeState = gameStore.subscribe((state) => renderGameState(state));

    renderGameState(gameStore);
    refreshInstances();
}

export function unmount() {
    unsubscribeLines?.();
    unsubscribeState?.();
    unsubscribeLines = null;
    unsubscribeState = null;
    inspectSeq++;
}
