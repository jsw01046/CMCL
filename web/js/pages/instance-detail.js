import { call } from '../rpc.js';
import { store } from '../store.js';
import { gameStore } from '../game-store.js';
import { downloadStore } from '../download-store.js';
import {
    el,
    badgeClass,
    typeLabel,
    loaderLabel,
    loaderBadgeClass,
    fmtBytes,
    formatError,
} from '../utils.js';
import {
    setStatus,
    setBreadcrumb,
    setTopActions,
    setSidebar,
    setSidebarFooter,
    confirmDialog,
    promptDialog,
    validateInstanceName,
} from '../ui.js';
import { createInstanceList, createInstanceSearch } from '../components/instance-list.js';

let ctx = null;
let id = '';
let list = null;
let inspectError = true;
let inspectSeq = 0;
let unsubscribeGame = null;

let detailTitle = null;
let detailBadge = null;
let detailLoaderBadge = null;
let detailList = null;
let detailError = null;
let isolateBox = null;
let isolateSavesBox = null;
let launchButton = null;
let renameButton = null;
let deleteButton = null;
let repairButton = null;
let unsubscribeDownload = null;

let repairCard = null;
let repairStage = null;
let repairMessage = null;
let repairFill = null;
let repairMeta = null;
let repairFile = null;
let repairCancel = null;

const rows = {};

function row(label) {
    const node = el('div');
    node.append(el('dt', { text: label }));
    const value = el('dd');
    node.append(value);
    detailList.append(node);
    return value;
}

function buildDetailCard() {
    const card = el('div', { className: 'card' });
    const head = el('div', { className: 'card-head' });
    detailTitle = el('h2', { text: id });
    detailLoaderBadge = el('span', { className: 'badge loader hidden' });
    detailBadge = el('span', { className: 'badge' });
    head.append(detailTitle, el('div', { className: 'badge-group' }, [
        detailLoaderBadge,
        detailBadge,
        el('button', {
            className: 'btn ghost small',
            text: '资源管理',
            type: 'button',
            onclick: () => {
                ctx.navigate('#/instances/' + encodeURIComponent(id) + '/resources');
            },
        }),
    ]));

    detailList = el('dl', { className: 'detail' });
    detailError = el('div', { className: 'alert hidden' });
    card.append(head, detailList, detailError);

    rows.type = row('版本类型');
    rows.loader = row('加载器');
    rows.vanilla = row('原版版本');
    rows.jar = row('主 JAR');
    rows.main = row('主类');
    rows.folder = row('实例文件夹');
    rows.gameDir = row('游戏目录');
    rows.saves = row('存档文件夹');
    rows.java = row('Java');
    rows.classpath = row('Classpath');
    rows.command = row('命令行');
    rows.natives = row('Natives');

    return card;
}

function buildSettingsCard() {
    const card = el('div', { className: 'card' });
    const head = el('div', { className: 'card-head' });
    head.append(el('h2', { text: '实例设置' }));

    const settingsRow = el('div', { className: 'setting-row' });
    const label = el('div');
    label.append(
        el('div', { className: 'setting-label', text: '使用隔离目录' }),
        el('div', {
            className: 'setting-desc',
            text: '开启后，mods、saves、config、资源包等数据保存在这个版本自己的文件夹里，不同版本互不影响。切换不会移动已有文件。',
        }),
    );

    isolateBox = el('input', { type: 'checkbox' });
    isolateBox.addEventListener('change', toggleIsolation);
    const control = el('label', { className: 'setting-control check' }, [
        isolateBox,
        el('span', { text: '启用' }),
    ]);

    settingsRow.append(label, control);

    const savesRow = el('div', { className: 'setting-row' });
    const savesLabel = el('div');
    savesLabel.append(
        el('div', { className: 'setting-label', text: '存档单独隔离' }),
        el('div', {
            className: 'setting-desc',
            text: '开启后，本实例的 saves 存档单独保存在 versions/<实例>/game/saves，切换完整隔离时路径不变；mods、config、资源包等仍与全局共享。开启“使用隔离目录”时，整个游戏目录独立，不再共享这些目录。',
        }),
    );
    isolateSavesBox = el('input', { type: 'checkbox' });
    isolateSavesBox.addEventListener('change', toggleSavesIsolation);
    const savesControl = el('label', { className: 'setting-control check' }, [
        isolateSavesBox,
        el('span', { text: '启用' }),
    ]);
    savesRow.append(savesLabel, savesControl);

    card.append(head, settingsRow, savesRow);
    return card;
}

function buildRepairCard() {
    repairCard = el('div', { className: 'card download-progress-card hidden' });
    const head = el('div', { className: 'card-head' });
    head.append(el('h2', { text: '修复进度' }));
    repairMessage = el('span', { className: 'muted progress-message', text: '' });
    head.append(repairMessage);

    repairStage = el('div', { className: 'progress-stage', text: '准备' });
    const bar = el('div', { className: 'progress' });
    repairFill = el('div', { className: 'progress-fill' });
    bar.append(repairFill);
    repairMeta = el('div', { className: 'progress-meta muted', text: '' });
    repairFile = el('div', { className: 'progress-file muted', text: '' });
    repairCancel = el('button', {
        className: 'btn ghost small',
        type: 'button',
        text: '取消',
        onclick: () => downloadStore.cancel(),
    });

    const body = el('div', { className: 'download-progress-body' }, [
        repairStage,
        bar,
        repairMeta,
        repairFile,
        repairCancel,
    ]);
    repairCard.append(head, body);
    return repairCard;
}

function renderRepairProgress(state) {
    if (!repairCard) return;
    const active = state.busy || state.done || Boolean(state.error) ||
        Boolean(state.stage);
    repairCard.classList.toggle('hidden', !active);
    if (!active) return;

    const percent = state.percent();
    repairFill.style.width = `${Math.max(0, Math.min(100, percent))}%`;
    repairFill.classList.toggle('error', Boolean(state.error));
    repairStage.textContent = state.error
        ? '修复失败'
        : `${state.stage || '准备'}${state.mode === 'repair' ? ' · 修复' : ''}`;
    repairMessage.textContent = state.error
        ? state.error.split('\n')[0]
        : (state.message || '');

    const parts = [];
    if (state.total > 0) parts.push(`${state.completed} / ${state.total} 个文件`);
    if (state.totalBytes > 0) {
        parts.push(`${fmtBytes(state.bytes)} / ${fmtBytes(state.totalBytes)}`);
    } else if (state.bytes > 0) {
        parts.push(fmtBytes(state.bytes));
    }
    repairMeta.textContent = parts.join(' · ');
    repairFile.textContent = state.file || '';
    repairCancel.classList.toggle('hidden', !state.busy);
}

function updateButtons() {
    if (!launchButton) return;
    const busy = gameStore.launching;
    const downloading = downloadStore.busy;
    const runningThis = gameStore.running &&
        (gameStore.version === id || !gameStore.version);

    launchButton.disabled = busy || downloading || gameStore.running || inspectError;
    launchButton.textContent = gameStore.running && gameStore.version === id
        ? '游戏运行中'
        : (busy ? '启动中…' : '启动游戏');
    renameButton.disabled = busy || downloading || runningThis;
    deleteButton.disabled = busy || downloading || runningThis;
    if (repairButton) {
        repairButton.disabled = busy || downloading || gameStore.running || inspectError;
        repairButton.textContent = downloading && downloadStore.mode === 'repair'
            ? '修复中…'
            : '修复';
    }
}

function applyInfo(data) {
    const displayName = store.instance(data.id)?.display_name || data.id || id;
    detailTitle.textContent = displayName;
    setBreadcrumb([
        { text: '版本管理', href: '#/instances' },
        displayName,
    ]);
    detailBadge.textContent = typeLabel(data.type);
    detailBadge.className = 'badge ' + badgeClass(data.type);

    const loader = data.loader || store.instance(data.id)?.loader || 'vanilla';
    detailLoaderBadge.textContent = loaderLabel(loader);
    detailLoaderBadge.className = loaderBadgeClass(loader);

    const java = data.java;
    rows.type.textContent = typeLabel(data.type);
    rows.loader.textContent = loaderLabel(loader);
    rows.vanilla.textContent = data.vanilla_id || '—';
    rows.jar.textContent = data.jar_id || '—';
    rows.main.textContent = data.main_class || '—';
    rows.folder.textContent = data.folder || '—';
    rows.folder.title = data.folder || '';
    rows.gameDir.textContent = data.game_dir
        ? data.game_dir +
          (data.isolate
              ? '（完整隔离）'
              : (data.isolate_saves === false ? '（共享）' : '（存档隔离）'))
        : '—';
    rows.gameDir.title = data.game_dir || '';
    rows.saves.textContent = data.saves_dir
        ? data.saves_dir +
          (data.isolate
              ? '（完整隔离）'
              : (data.isolate_saves === false ? '（共享）' : '（单独隔离）'))
        : '—';
    rows.saves.title = data.saves_dir || '';
    rows.java.textContent = java
        ? `Java ${java.version}（${java.bits} 位）` +
          (data.required_java ? ` · 要求 Java ${data.required_java}+` : '')
        : '自动检测';
    rows.java.title = java ? java.path : '';
    rows.classpath.textContent = data.classpath_count != null
        ? `${data.classpath_count} 项`
        : '—';
    rows.command.textContent = data.command_length != null
        ? `${data.command_length} 字符`
        : '—';
    rows.natives.textContent = data.natives_dir || '—';
    rows.natives.title = data.natives_dir || '';

    isolateBox.checked = Boolean(data.isolate);
    isolateSavesBox.checked = data.isolate_saves !== false;
    isolateSavesBox.disabled = Boolean(data.isolate);

    if (data.error) {
        inspectError = true;
        detailError.textContent = formatError(data.error);
        detailError.classList.remove('hidden');
        setStatus('参数检查未通过');
    } else {
        inspectError = false;
        detailError.classList.add('hidden');
        setStatus('就绪');
    }
    updateButtons();
}

async function load() {
    const token = ++inspectSeq;
    setStatus(`正在检查 ${id}…`);
    updateButtons();

    try {
        const data = await call('instance.inspect', store.launchParams({ version: id }));
        if (token !== inspectSeq) return;
        applyInfo(data);
    } catch (error) {
        if (token !== inspectSeq) return;
        inspectError = true;
        detailError.textContent = formatError(error);
        detailError.classList.remove('hidden');
        setStatus('加载失败');
        updateButtons();
    }
}

async function loadSidebar() {
    try {
        if (!store.instances.length) await store.refreshInstances();
        list.setData(store.instances, id);
    } catch (_) {
        // 详情本身仍可独立加载。
    }
}

async function toggleIsolation() {
    const value = isolateBox.checked;
    isolateBox.disabled = true;
    setStatus(value ? '正在开启隔离…' : '正在关闭隔离…');

    try {
        await call('instance.settings.set', {
            mc_dir: store.mcDir,
            id,
            isolate: value,
        });
        try {
            await store.refreshInstances();
            list.setData(store.instances, id);
        } catch (_) {
            // 列表刷新失败不影响已保存的设置。
        }
        await load();
        setStatus(value ? '已开启隔离目录' : '已关闭隔离目录');
    } catch (error) {
        isolateBox.checked = !value;
        setStatus('保存失败：' + formatError(error).split('\n')[0]);
    } finally {
        isolateBox.disabled = false;
    }
}

async function toggleSavesIsolation() {
    const value = isolateSavesBox.checked;
    isolateSavesBox.disabled = true;
    setStatus(value ? '正在开启存档隔离…' : '正在关闭存档隔离…');

    try {
        await call('instance.settings.set', {
            mc_dir: store.mcDir,
            id,
            isolate_saves: value,
        });
        await load();
        setStatus(value ? '已开启存档单独隔离' : '已关闭存档单独隔离');
    } catch (error) {
        isolateSavesBox.checked = !value;
        setStatus('保存失败：' + formatError(error).split('\n')[0]);
    } finally {
        isolateSavesBox.disabled = false;
    }
}

function launchVersion() {
    if (inspectError || gameStore.running) return;
    store.selected = id;
    store.save();
    ctx.navigate('#/launch');
}

async function repairVersion() {
    if (downloadStore.busy || gameStore.running) return;
    setStatus('正在校验并修复实例文件…');
    try {
        await downloadStore.start('repair', {
            mc_dir: store.mcDir,
            version: id,
            source: store.settings.downloadSource,
        });
        try {
            await store.refreshInstances();
            list.setData(store.instances, id);
        } catch (_) {
            // 修复已完成，列表稍后刷新即可。
        }
        await load();
        setStatus('修复完成');
    } catch (error) {
        setStatus('修复失败：' + formatError(error).split('\n')[0]);
    }
}

async function openFolder() {
    try {
        await call('instance.open_folder', { mc_dir: store.mcDir, id });
        setStatus('已打开实例文件夹');
    } catch (error) {
        setStatus('打开目录失败：' + formatError(error).split('\n')[0]);
    }
}

async function openSaves() {
    try {
        await call('instance.open_saves', { mc_dir: store.mcDir, id });
        setStatus('已打开存档文件夹');
    } catch (error) {
        setStatus('打开存档文件夹失败：' + formatError(error).split('\n')[0]);
    }
}

async function renameVersion() {
    const next = await promptDialog({
        title: '重命名版本',
        message: '会同时修改版本文件夹、JSON 文件和 JAR 文件名，并修正引用它的继承版本。',
        label: '新的版本名',
        value: id,
        confirmText: '重命名',
        validate: validateInstanceName,
    });
    if (!next || next === id) return;

    if (gameStore.running && (gameStore.version === id || !gameStore.version)) {
        setStatus('该版本正在运行，无法重命名');
        return;
    }

    setStatus(`正在重命名 ${id}…`);
    try {
        const result = await call('instance.rename', {
            mc_dir: store.mcDir,
            id,
            new_id: next,
        });
        const newId = result.id || next;
        store.selected = newId;
        try {
            await store.refreshInstances();
        } catch (_) {
            // 已重命名成功，列表稍后刷新即可。
        }
        setStatus(`已重命名为 ${newId}`);
        ctx.navigate('#/instances/' + encodeURIComponent(newId));
    } catch (error) {
        setStatus('重命名失败：' + formatError(error).split('\n')[0]);
    }
}

async function deleteVersion() {
    if (gameStore.running && (gameStore.version === id || !gameStore.version)) {
        setStatus('该版本正在运行，无法删除');
        return;
    }

    const confirmed = await confirmDialog({
        title: `删除版本 ${id}？`,
        message: '版本文件夹会被移动到回收站，其中的存档、模组和配置也会一起移走。\n可以从 Windows 回收站还原。',
        confirmText: '移到回收站',
        danger: true,
    });
    if (!confirmed) return;

    setStatus(`正在删除 ${id}…`);
    try {
        await call('instance.delete', { mc_dir: store.mcDir, id });
        if (store.selected === id) store.selected = null;
        try {
            await store.refreshInstances();
        } catch (_) {
            // 已删除成功，列表稍后刷新即可。
        }
        setStatus(`已删除 ${id}，可在回收站还原`);
        ctx.navigate('#/instances');
    } catch (error) {
        setStatus('删除失败：' + formatError(error).split('\n')[0]);
    }
}

async function refreshAll() {
    await loadSidebar();
    await load();
}

export function mount(context, params) {
    ctx = context;
    id = params?.id || '';
    store.selected = id;
    store.save();
    inspectSeq++;

    setBreadcrumb([
        { text: '版本管理', href: '#/instances' },
        id || '版本详情',
    ]);
    setStatus('正在加载版本…');

    launchButton = el('button', {
        className: 'btn primary',
        text: '启动游戏',
        type: 'button',
        onclick: launchVersion,
    });
    repairButton = el('button', {
        className: 'btn ghost',
        text: '修复',
        type: 'button',
        onclick: repairVersion,
    });
    renameButton = el('button', {
        className: 'btn ghost',
        text: '重命名',
        type: 'button',
        onclick: renameVersion,
    });
    deleteButton = el('button', {
        className: 'btn danger',
        text: '删除',
        type: 'button',
        onclick: deleteVersion,
    });
    setTopActions([
        el('button', {
            className: 'btn ghost',
            text: '打开文件夹',
            type: 'button',
            onclick: openFolder,
        }),
        el('button', {
            className: 'btn ghost',
            text: '存档文件夹',
            type: 'button',
            onclick: openSaves,
        }),
        repairButton,
        renameButton,
        deleteButton,
        launchButton,
    ]);

    list = createInstanceList({
        onSelect: (nextId) => {
            if (nextId !== id) {
                ctx.navigate('#/instances/' + encodeURIComponent(nextId));
            }
        },
    });
    setSidebar([createInstanceSearch(list), list.element]);
    setSidebarFooter([
        el('button', {
            className: 'btn ghost small',
            text: '刷新',
            type: 'button',
            onclick: refreshAll,
        }),
    ]);

    ctx.page.append(buildDetailCard(), buildSettingsCard(), buildRepairCard());

    unsubscribeGame = gameStore.subscribe(updateButtons);
    unsubscribeDownload = downloadStore.subscribe(renderRepairProgress);

    (async () => {
        await loadSidebar();
        await load();
    })();
}

export function unmount() {
    unsubscribeGame?.();
    unsubscribeGame = null;
    unsubscribeDownload?.();
    unsubscribeDownload = null;
    inspectSeq++;
    ctx = null;
}
