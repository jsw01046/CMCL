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
    LOADER_ORDER,
    formatError,
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

let ctx = null;
let list = null;
let tbody = null;
let stats = {};
let refreshButton = null;
let loaderTabs = null;
let tableTitle = null;
let loaderFilter = '';

function statCard(label) {
    const node = el('div', { className: 'stat' });
    node.append(el('div', { className: 'label', text: label }));
    const value = el('div', { className: 'value', text: '—' });
    node.append(value);
    return { node, value };
}

function buildStats() {
    stats.total = statCard('版本总数');
    stats.valid = statCard('可用版本');
    stats.isolate = statCard('隔离实例');
    stats.path = statCard('游戏目录');
    stats.path.value.classList.add('small');
    return el('div', { className: 'stat-grid' }, [
        stats.total.node,
        stats.valid.node,
        stats.isolate.node,
        stats.path.node,
    ]);
}

function loaderCounts() {
    const counts = new Map();
    let other = 0;
    for (const item of store.instances) {
        if (LOADER_ORDER.includes(item.loader)) {
            counts.set(item.loader, (counts.get(item.loader) || 0) + 1);
        } else {
            other++;
        }
    }
    return { counts, other };
}

function renderLoaderTabs() {
    if (!loaderTabs) return;
    clear(loaderTabs);

    const { counts, other } = loaderCounts();
    const entries = [
        { value: '', label: '全部', count: store.instances.length },
        ...LOADER_ORDER
            .filter((loader) => counts.has(loader))
            .map((loader) => ({
                value: loader,
                label: loaderLabel(loader),
                count: counts.get(loader),
            })),
    ];
    if (other) entries.push({ value: 'other', label: '其他', count: other });

    for (const entry of entries) {
        loaderTabs.append(el('button', {
            className: 'section-tab' + (entry.value === loaderFilter ? ' active' : ''),
            type: 'button',
            text: `${entry.label} ${entry.count}`,
            onclick: () => {
                loaderFilter = entry.value;
                renderLoaderTabs();
                renderTable();
            },
        }));
    }
}

function buildLoaderTabs() {
    loaderTabs = el('div', { className: 'section-tabs loader-tabs' });
    return loaderTabs;
}

function buildTable() {
    const card = el('div', { className: 'card table-card' });
    const head = el('div', { className: 'card-head' });
    tableTitle = el('h2', { text: '全部版本' });
    head.append(tableTitle);

    const table = el('table', { className: 'instance-table' });
    const thead = el('thead');
    const headRow = el('tr');
    for (const [title, className] of [
        ['版本', 'col-name'],
        ['加载器', 'col-loader'],
        ['类型', 'col-type'],
        ['继承 / 主类', 'col-sub'],
        ['隔离', 'col-isolate'],
        ['操作', 'col-actions'],
    ]) {
        headRow.append(el('th', { text: title, className }));
    }
    thead.append(headRow);

    tbody = el('tbody');
    table.append(thead, tbody);

    const scroll = el('div', { className: 'table-scroll' }, [table]);
    card.append(head, scroll);
    return card;
}

function renderTable() {
    clear(tbody);
    if (tableTitle) {
        tableTitle.textContent = loaderFilter
            ? `${loaderLabel(loaderFilter)} 版本`
            : '全部版本';
    }

    if (!store.instances.length) {
        const row = el('tr');
        const cell = el('td', { className: 'muted', text: '没有找到任何版本。' });
        cell.colSpan = 6;
        row.append(cell);
        tbody.append(row);
        return;
    }

    const visible = store.instances.filter((item) => {
        if (loaderFilter === 'other') return !LOADER_ORDER.includes(item.loader);
        return !loaderFilter || item.loader === loaderFilter;
    });

    if (!visible.length) {
        const row = el('tr');
        const cell = el('td', {
            className: 'muted',
            text: '这个分类下还没有版本。',
        });
        cell.colSpan = 6;
        row.append(cell);
        tbody.append(row);
        return;
    }

    for (const item of visible) {
        const row = el('tr');

        const nameCell = el('td', {
            className: 'name-cell col-name',
            title: item.id,
        });
        nameCell.append(el('span', { className: 'name-main', text: item.id }));
        const detailText = item.valid
            ? (item.inherits_from ? '继承 ' + item.inherits_from : (item.main_class || '—'))
            : (item.error || '版本文件缺失');
        nameCell.append(el('span', {
            className: 'name-sub',
            text: `${item.isolate ? '隔离' : '共享'} · ${detailText}`,
        }));
        nameCell.style.cursor = 'pointer';
        nameCell.addEventListener('click', () => openDetail(item.id));

        const loaderCell = el('td', { className: 'col-loader' });
        if (item.valid) {
            loaderCell.append(el('span', {
                className: loaderBadgeClass(item.loader),
                text: loaderLabel(item.loader),
            }));
        } else {
            loaderCell.append(el('span', { className: 'muted', text: '—' }));
        }

        const typeCell = el('td', { className: 'col-type' });
        typeCell.append(el('span', {
            className: 'badge ' + (item.valid ? badgeClass(item.type) : 'error'),
            text: item.valid ? typeLabel(item.type) : '无效',
        }));

        const subCell = el('td', {
            className: 'mono col-sub',
            title: item.valid ? (item.main_class || item.inherits_from || '') : (item.error || ''),
            text: item.valid
                ? (item.inherits_from ? '继承 ' + item.inherits_from : (item.main_class || '—'))
                : (item.error || '版本文件缺失'),
        });

        const isolateCell = el('td', { className: 'col-isolate' });
        if (item.isolate) {
            isolateCell.append(el('span', { className: 'badge isolate', text: '隔离' }));
        } else {
            isolateCell.append(el('span', { className: 'muted', text: '共享' }));
        }

        const actionCell = el('td', { className: 'col-actions' });
        const actions = el('div', { className: 'actions' });
        actions.append(
            el('button', {
                className: 'btn ghost small',
                text: '详情',
                type: 'button',
                onclick: () => openDetail(item.id),
            }),
            el('button', {
                className: 'btn ghost small',
                text: '打开目录',
                type: 'button',
                onclick: () => openFolder(item),
            }),
            el('button', {
                className: 'btn ghost small',
                text: '删除',
                type: 'button',
                onclick: () => removeInstance(item),
            }),
        );
        actionCell.append(actions);

        row.append(nameCell, loaderCell, typeCell, subCell, isolateCell, actionCell);
        tbody.append(row);
    }
}

function updateStats() {
    stats.total.value.textContent = String(store.instances.length);
    stats.valid.value.textContent = String(
        store.instances.filter((item) => item.valid).length);
    stats.isolate.value.textContent = String(
        store.instances.filter((item) => item.isolate).length);
    stats.path.value.textContent = store.mcDir || '—';
    stats.path.value.title = store.mcDir || '';
}

function openDetail(id) {
    ctx.navigate('#/instances/' + encodeURIComponent(id));
}

async function openFolder(item) {
    try {
        await call('instance.open_folder', { mc_dir: store.mcDir, id: item.id });
        setStatus(`已打开 ${item.id} 的文件夹`);
    } catch (error) {
        setStatus('打开目录失败：' + formatError(error).split('\n')[0]);
    }
}

async function removeInstance(item) {
    if (gameStore.running && gameStore.version === item.id) {
        setStatus('该版本正在运行，请先停止游戏');
        return;
    }

    const confirmed = await confirmDialog({
        title: `删除版本 ${item.id}？`,
        message: '版本文件夹会被移动到回收站，其中的存档、模组和配置也会一起移走。\n可以从 Windows 回收站还原。',
        confirmText: '移到回收站',
        danger: true,
    });
    if (!confirmed) return;

    setStatus(`正在删除 ${item.id}…`);
    try {
        await call('instance.delete', { mc_dir: store.mcDir, id: item.id });
        if (store.selected === item.id) store.selected = null;
        setStatus(`已删除 ${item.id}，可在回收站还原`);
        await refresh();
    } catch (error) {
        setStatus('删除失败：' + formatError(error).split('\n')[0]);
    }
}

async function refresh() {
    if (refreshButton) refreshButton.disabled = true;
    setStatus('正在扫描版本…');
    try {
        const instances = await store.refreshInstances();
        list.setData(instances);

        const { counts, other } = loaderCounts();
        if (loaderFilter === 'other' && !other) loaderFilter = '';
        else if (loaderFilter && loaderFilter !== 'other' && !counts.has(loaderFilter)) {
            loaderFilter = '';
        }

        renderLoaderTabs();
        renderTable();
        updateStats();
        setStatus(`共 ${instances.length} 个版本`);
    } catch (error) {
        setStatus('扫描失败：' + formatError(error).split('\n')[0]);
        clear(tbody);
        const row = el('tr');
        const cell = el('td', { className: 'muted', text: formatError(error) });
        cell.colSpan = 6;
        row.append(cell);
        tbody.append(row);
    } finally {
        if (refreshButton) refreshButton.disabled = false;
    }
}

export function mount(context) {
    ctx = context;

    setBreadcrumb('版本管理');
    setStatus('正在扫描版本…');

    setTopActions([
        el('button', {
            className: 'btn ghost',
            text: '导入整合包',
            type: 'button',
            disabled: true,
            title: '下载与整合包功能将在 M3 提供',
        }),
    ]);

    list = createInstanceList({
        onSelect: (id) => openDetail(id),
        onActivate: (id) => openDetail(id),
    });
    setSidebar([createInstanceSearch(list), list.element]);

    refreshButton = el('button', {
        className: 'btn ghost small',
        text: '刷新',
        type: 'button',
        onclick: refresh,
    });
    setSidebarFooter([refreshButton]);

    ctx.page.append(buildStats(), buildLoaderTabs(), buildTable());
    refresh();
}

export function unmount() {
    ctx = null;
}
