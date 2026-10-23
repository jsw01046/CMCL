import { call } from '../rpc.js';
import { store } from '../store.js';
import {
    el,
    clear,
    fmtBytes,
    formatError,
    badgeClass,
    typeLabel,
    loaderLabel,
    loaderBadgeClass,
} from '../utils.js';
import {
    setStatus,
    setBreadcrumb,
    setTopActions,
    setSidebar,
    setSidebarFooter,
    confirmDialog,
} from '../ui.js';
import { createInstanceList, createInstanceSearch } from '../components/instance-list.js';

const KINDS = [
    { key: 'mods', label: 'Mod', icon: 'M', section: 'mod' },
    { key: 'resourcepacks', label: '资源包', icon: '资', section: 'resourcepack' },
    { key: 'shaderpacks', label: '光影包', icon: '影', section: 'shader' },
    { key: 'datapacks', label: '数据包', icon: '数', section: 'datapack' },
];

let ctx = null;
let id = '';
let kind = 'mods';
let list = null;
let entries = [];
let filter = '';
let loading = false;
let errorText = '';
let dirText = '';
let scopeInfo = { isolate: false, isolate_saves: true, shared: false };
let seq = 0;
let acting = false;
let elements = {};

function currentKind() {
    return KINDS.find((item) => item.key === kind) || KINDS[0];
}

function displayName(entry) {
    return entry.name.endsWith('.disabled')
        ? entry.name.slice(0, -'.disabled'.length)
        : entry.name;
}

function formatTime(seconds) {
    if (!seconds) return '—';
    const date = new Date(Number(seconds) * 1000);
    if (Number.isNaN(date.getTime())) return '—';
    const pad = (value) => String(value).padStart(2, '0');
    return `${date.getFullYear()}-${pad(date.getMonth() + 1)}-${pad(date.getDate())} ` +
        `${pad(date.getHours())}:${pad(date.getMinutes())}`;
}

function fileIcon(entry) {
    if (entry.directory) return '夹';
    const name = displayName(entry).toLowerCase();
    if (name.endsWith('.jar')) return 'J';
    if (name.endsWith('.zip')) return 'Z';
    return '·';
}

function buildHeader() {
    const card = el('div', { className: 'card rm-head' });
    const info = el('div', { className: 'rm-head-text' });
    const title = el('div', { className: 'rm-title' });
    const name = el('h2', { text: id });
    const loaderBadge = el('span', { className: 'badge loader hidden' });
    const typeBadge = el('span', { className: 'badge' });
    title.append(name, loaderBadge, typeBadge);

    const subtitle = el('div', {
        className: 'rm-subtitle muted',
        text: '管理这个实例已安装的 Mod、资源包、光影包和数据包。',
    });
    info.append(title, subtitle);

    const actions = el('div', { className: 'rm-head-actions' });
    actions.append(
        el('button', {
            className: 'btn primary small',
            type: 'button',
            text: '去 Modrinth 下载',
            onclick: () => {
                store.selected = id;
                store.save();
                ctx.navigate('#/download/' + currentKind().section);
            },
        }),
        el('button', {
            className: 'btn ghost small',
            type: 'button',
            text: '打开文件夹',
            onclick: openFolder,
        }),
    );

    elements.loaderBadge = loaderBadge;
    elements.typeBadge = typeBadge;
    card.append(info, actions);
    return card;
}

function buildListCard() {
    const card = el('div', { className: 'card rm-card' });
    const head = el('div', { className: 'card-head' });
    const count = el('span', { className: 'rm-count muted', text: '0 项' });
    elements.count = count;
    head.append(el('h2', { text: '已安装资源' }), count);

    const tabs = el('div', { className: 'rm-tabs' });
    elements.tabs = {};
    for (const item of KINDS) {
        const button = el('button', {
            className: 'rm-tab' + (item.key === kind ? ' active' : ''),
            type: 'button',
        });
        button.append(
            el('span', { className: 'rm-tab-icon', text: item.icon }),
            el('span', { text: item.label }),
        );
        button.addEventListener('click', () => switchKind(item.key));
        elements.tabs[item.key] = button;
        tabs.append(button);
    }

    const toolbar = el('div', { className: 'rm-toolbar' });
    const search = el('input', {
        type: 'search',
        placeholder: '搜索文件名',
        spellcheck: 'false',
    });
    search.addEventListener('input', () => {
        filter = search.value.trim().toLowerCase();
        render();
    });
    elements.search = search;
    toolbar.append(
        search,
        el('button', {
            className: 'btn ghost small',
            type: 'button',
            text: '刷新',
            onclick: () => loadResources(false),
        }),
    );

    const scope = el('div', { className: 'rm-scope muted' });
    elements.scope = scope;

    const wrap = el('div', { className: 'rm-table-wrap' });
    const table = el('table', { className: 'rm-table' });
    const thead = el('thead');
    const headerRow = el('tr');
    for (const text of ['名称', '状态', '大小', '修改时间', '操作']) {
        headerRow.append(el('th', { text }));
    }
    thead.append(headerRow);
    const tbody = el('tbody');
    table.append(thead, tbody);
    wrap.append(table);
    elements.wrap = wrap;
    elements.tbody = tbody;

    const empty = el('div', { className: 'rm-empty hidden' });
    elements.empty = empty;

    const loadingNode = el('div', { className: 'rm-loading hidden' });
    for (let i = 0; i < 4; i += 1) {
        loadingNode.append(el('div', { className: 'rm-skeleton' }));
    }
    elements.loading = loadingNode;

    const error = el('div', { className: 'alert hidden' });
    elements.error = error;

    card.append(head, tabs, scope, toolbar, wrap, empty, loadingNode, error);
    return card;
}

function switchKind(next) {
    if (next === kind || !elements.tabs) return;
    kind = next;
    filter = '';
    if (elements.search) elements.search.value = '';
    for (const [key, button] of Object.entries(elements.tabs)) {
        button.classList.toggle('active', key === kind);
    }
    loadResources(true);
}

function renderScope() {
    if (!elements.scope) return;
    if (!dirText) {
        elements.scope.textContent = '';
        return;
    }
    let mode = ' · 实例独立目录';
    if (scopeInfo.isolate) mode = ' · 实例独立目录';
    else if (scopeInfo.shared && scopeInfo.isolate_saves) mode = ' · 与其它实例共享';
    else if (scopeInfo.shared) mode = ' · 全局目录';
    elements.scope.textContent = dirText + mode;
    elements.scope.title = dirText;
}

function emptyActions() {
    const box = el('div', { className: 'rm-empty-actions' });
    box.append(
        el('button', {
            className: 'btn ghost small',
            type: 'button',
            text: '打开文件夹',
            onclick: openFolder,
        }),
        el('button', {
            className: 'btn primary small',
            type: 'button',
            text: '去 Modrinth 下载',
            onclick: () => {
                store.selected = id;
                store.save();
                ctx.navigate('#/download/' + currentKind().section);
            },
        }),
    );
    return box;
}

function fillEmpty() {
    if (!elements.empty) return;
    clear(elements.empty);
    const item = currentKind();
    elements.empty.append(
        el('div', { className: 'rm-empty-icon', text: item.icon }),
        el('h3', {
            text: filter ? '没有匹配的文件' : `还没有安装${item.label}`,
        }),
        el('p', {
            text: filter
                ? '换个关键词试试。'
                : `可以把${item.label}文件放进这个文件夹，或直接从 Modrinth 下载。`,
        }),
    );
    if (!filter) elements.empty.append(emptyActions());
}

function renderRows() {
    if (!elements.tbody) return;
    clear(elements.tbody);

    const needle = filter;
    const visible = entries.filter((entry) => {
        if (!needle) return true;
        return displayName(entry).toLowerCase().includes(needle) ||
            entry.name.toLowerCase().includes(needle);
    });

    elements.count.textContent = needle
        ? `${visible.length} / ${entries.length} 项`
        : `${entries.length} 项`;

    if (!loading && !errorText && !visible.length) fillEmpty();

    for (const entry of visible) {
        const tr = el('tr', { className: entry.enabled ? '' : 'disabled' });

        const nameCell = el('td');
        const nameBox = el('div', { className: 'rm-name' });
        nameBox.append(el('span', {
            className: 'rm-file-icon' + (entry.directory ? ' folder' : ''),
            text: fileIcon(entry),
        }));
        const nameText = el('div', { className: 'rm-name-text' });
        nameText.append(el('span', {
            className: 'rm-file-name',
            text: displayName(entry),
            title: entry.name,
        }));
        if (entry.directory) {
            nameText.append(el('span', {
                className: 'rm-file-dir muted',
                text: '文件夹',
            }));
        }
        nameBox.append(nameText);
        nameCell.append(nameBox);

        const status = el('td');
        status.append(el('span', {
            className: 'rm-state ' + (entry.enabled ? 'on' : 'off'),
            text: entry.enabled ? '已启用' : '已禁用',
        }));

        const size = el('td', {
            className: 'rm-size muted',
            text: entry.directory ? '—' : fmtBytes(entry.size),
        });
        const modified = el('td', {
            className: 'rm-modified muted',
            text: formatTime(entry.modified),
        });

        const actionsCell = el('td');
        const actions = el('div', { className: 'rm-actions' });
        if (!entry.directory) {
            actions.append(el('button', {
                className: 'btn ghost small',
                type: 'button',
                text: entry.enabled ? '禁用' : '启用',
                onclick: () => toggleEntry(entry),
            }));
        }
        actions.append(el('button', {
            className: 'btn danger small',
            type: 'button',
            text: '删除',
            onclick: () => deleteEntry(entry),
        }));
        actionsCell.append(actions);

        tr.append(nameCell, status, size, modified, actionsCell);
        elements.tbody.append(tr);
    }

    elements.wrap?.classList.toggle(
        'hidden',
        loading || Boolean(errorText) || !visible.length,
    );
    elements.empty?.classList.toggle(
        'hidden',
        loading || Boolean(errorText) || Boolean(visible.length),
    );
}

function render() {
    if (!elements.tbody) return;
    renderScope();
    elements.loading?.classList.toggle('hidden', !loading);
    elements.error?.classList.toggle('hidden', !errorText);
    if (elements.error) elements.error.textContent = errorText;

    renderRows();
}

async function loadResources(showLoading) {
    if (!id) return;
    const token = ++seq;
    loading = Boolean(showLoading);
    errorText = '';
    render();
    setStatus(`正在读取${currentKind().label}…`);

    try {
        const result = await call('instance.resources.list', {
            mc_dir: store.mcDir,
            id,
            kind,
        });
        if (token !== seq) return;

        entries = Array.isArray(result.entries) ? result.entries : [];
        dirText = result.dir || '';
        scopeInfo = {
            isolate: Boolean(result.isolate),
            isolate_saves: result.isolate_saves !== false,
            shared: Boolean(result.shared),
        };
        loading = false;
        errorText = '';
        render();
        setStatus(`${currentKind().label}：${entries.length} 项`);
    } catch (error) {
        if (token !== seq) return;
        entries = [];
        loading = false;
        errorText = formatError(error);
        render();
        setStatus('读取失败：' + errorText.split('\n')[0]);
    }
}

async function toggleEntry(entry) {
    if (acting) return;
    acting = true;
    const name = displayName(entry);
    setStatus(`正在${entry.enabled ? '禁用' : '启用'} ${name}…`);
    try {
        await call('instance.resources.toggle', {
            mc_dir: store.mcDir,
            id,
            kind,
            name: entry.name,
        });
        await loadResources(false);
        setStatus(`${entry.enabled ? '已禁用' : '已启用'} ${name}`);
    } catch (error) {
        setStatus('操作失败：' + formatError(error).split('\n')[0]);
    } finally {
        acting = false;
    }
}

async function deleteEntry(entry) {
    if (acting) return;
    const name = displayName(entry);
    const confirmed = await confirmDialog({
        title: `删除 ${name}？`,
        message: '文件会被移动到 Windows 回收站，可以从回收站还原。',
        confirmText: '移到回收站',
        danger: true,
    });
    if (!confirmed || acting) return;

    acting = true;
    setStatus(`正在删除 ${name}…`);
    try {
        await call('instance.resources.delete', {
            mc_dir: store.mcDir,
            id,
            kind,
            name: entry.name,
        });
        await loadResources(false);
        setStatus(`已删除 ${name}，可在回收站还原`);
    } catch (error) {
        setStatus('删除失败：' + formatError(error).split('\n')[0]);
    } finally {
        acting = false;
    }
}

async function openFolder() {
    try {
        await call('instance.resources.open', {
            mc_dir: store.mcDir,
            id,
            kind,
        });
        setStatus('已打开资源文件夹');
    } catch (error) {
        setStatus('打开文件夹失败：' + formatError(error).split('\n')[0]);
    }
}

async function loadSummary() {
    try {
        if (!store.instances.length) await store.refreshInstances();
        if (list) list.setData(store.instances, id);
        const info = store.instance(id);
        if (!info || !elements.loaderBadge) return;
        const loader = info.loader || 'vanilla';
        elements.loaderBadge.textContent = loaderLabel(loader);
        elements.loaderBadge.className = loaderBadgeClass(loader);
        elements.typeBadge.textContent = typeLabel(info.type);
        elements.typeBadge.className = 'badge ' + badgeClass(info.type);
    } catch (_) {
        // 侧栏刷新失败不影响资源列表。
    }
}

export function mount(context, params) {
    ctx = context;
    id = params?.id || '';
    kind = 'mods';
    entries = [];
    filter = '';
    loading = false;
    acting = false;
    errorText = '';
    dirText = '';
    scopeInfo = { isolate: false, isolate_saves: true, shared: false };
    elements = {};
    seq++;

    store.selected = id;
    store.save();

    setBreadcrumb([
        { text: '版本管理', href: '#/instances' },
        { text: id, href: '#/instances/' + encodeURIComponent(id) },
        '资源管理',
    ]);
    setTopActions([]);
    setStatus('正在加载资源…');
    ctx.page.scrollTop = 0;

    ctx.page.append(buildHeader(), buildListCard());

    list = createInstanceList({
        onSelect: (nextId) => {
            if (nextId !== id) {
                ctx.navigate('#/instances/' + encodeURIComponent(nextId) + '/resources');
            }
        },
    });
    setSidebar([createInstanceSearch(list), list.element]);
    setSidebarFooter([
        el('button', {
            className: 'btn ghost small',
            type: 'button',
            text: '返回版本详情',
            onclick: () => ctx.navigate('#/instances/' + encodeURIComponent(id)),
        }),
    ]);

    (async () => {
        await loadSummary();
        await loadResources(true);
    })();
}

export function unmount() {
    seq++;
    entries = [];
    acting = false;
    elements = {};
    list = null;
    ctx = null;
}
