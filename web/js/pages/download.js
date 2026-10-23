import {
    el,
    clear,
    fmtBytes,
    fmtDuration,
    typeLabel,
    badgeClass,
    loaderLabel,
    formatError,
} from '../utils.js';
import { call } from '../rpc.js';
import { store } from '../store.js';
import { downloadStore } from '../download-store.js';
import {
    setStatus,
    setBreadcrumb,
    setTopActions,
    setSidebar,
    setSidebarFooter,
    validateInstanceName,
} from '../ui.js';

const SECTIONS = {
    install: {
        label: '原版游戏',
        icon: 'MC',
        title: '安装游戏',
        kind: 'install',
    },
    mod: {
        label: 'Mod',
        icon: 'M',
        title: '搜索 Mod',
        kind: 'resource',
        itemName: 'Mod',
        showLoader: true,
        categories: [
            '全部', '世界元素', '科技', '魔法', '冒险', '装饰',
            '实用', '性能优化', '信息显示', '支持库',
        ],
    },
    modpack: {
        label: '整合包',
        icon: '包',
        title: '搜索整合包',
        kind: 'resource',
        itemName: '整合包',
        showLoader: false,
        categories: [
            '全部', '多人', '性能优化', '硬核', '战斗', '任务',
            '科技', '魔法', '冒险', '水槽包', '空岛', '轻量整合', '大型整合',
        ],
    },
    datapack: {
        label: '数据包',
        icon: '数',
        title: '搜索数据包',
        kind: 'resource',
        itemName: '数据包',
        showLoader: false,
        categories: [
            '全部', '世界元素', '生物群系', '维度', '矿物与资源',
            '天然结构', '游戏机制', '实用',
        ],
    },
    resourcepack: {
        label: '资源包',
        icon: '资',
        title: '搜索资源包',
        kind: 'resource',
        itemName: '资源包',
        showLoader: false,
        categories: [
            '全部', '写实', '卡通', '现代', '中世纪', '简约',
            '16x', '32x', '64x', '128x 以上',
        ],
    },
    shader: {
        label: '光影包',
        icon: '影',
        title: '搜索光影包',
        kind: 'resource',
        itemName: '光影包',
        showLoader: false,
        categories: [
            '全部', '写实', '梦幻', '卡通', '性能', '光影整合',
        ],
    },
};

const COMMUNITY_ORDER = ['mod', 'modpack', 'datapack', 'resourcepack', 'shader'];

const LOADERS = [
    { key: 'forge', letter: 'F', name: 'Forge', desc: '经典模组加载器，适合大多数整合包。', stage: 'M4', available: true },
    { key: 'neoforge', letter: 'N', name: 'NeoForge', desc: 'Forge 的现代分支，主要支持 1.20.2 及以上版本。', stage: 'M4', available: true },
    { key: 'fabric', letter: 'F', name: 'Fabric', desc: '轻量、更新快的模组加载器。', stage: 'M4', available: true },
    { key: 'quilt', letter: 'Q', name: 'Quilt', desc: 'Fabric 生态的现代分支。', stage: 'M4', available: true },
    { key: 'fabric-api', letter: 'A', name: 'Fabric API', desc: '大多数 Fabric Mod 的前置库。', stage: '后续', available: false },
    { key: 'optifine', letter: 'O', name: 'OptiFine', desc: '光影与性能优化。与部分 Mod 兼容性有限。', stage: '后续', available: false },
];

const SOURCE_OPTIONS = [
    ['auto', '自动（官方优先，速度慢时切换镜像）'],
    ['official', '仅官方源'],
    ['mirror', '仅镜像源'],
];

const VERSION_TYPE_TABS = [
    ['release', '正式版'],
    ['snapshot', '快照'],
    ['old', '旧版'],
];

const MAX_RENDERED_VERSIONS = 300;

const STAGE_STEPS = [
    ['版本 JSON', '版本信息'],
    ['客户端', '客户端'],
    ['库文件', '运行库'],
    ['游戏资源索引', '资源索引'],
    ['游戏资源', '游戏资源'],
    ['加载器', '加载器'],
    ['完成', '完成'],
];

const STAGE_LABELS = {
    '准备': '准备中',
    '版本 JSON': '下载版本信息',
    '客户端': '下载客户端',
    '库文件': '下载运行库',
    '游戏资源索引': '下载资源索引',
    '游戏资源': '下载游戏资源',
    '加载器': '安装模组加载器',
    '完成': '下载完成',
    '失败': '下载失败',
};

function stageLabel(stage) {
    return STAGE_LABELS[stage] || stage || '准备中';
}

function shortFile(file) {
    if (!file) return '';
    let path = String(file);
    let source = '';
    const match = path.match(/^(.*?)\s+\[([^\]]+)\]$/);
    if (match) {
        path = match[1];
        source = match[2];
    }
    const parts = path.split(/[\\/]/).filter(Boolean);
    const tail = parts.slice(-3).join('/');
    return source ? `${tail}  ·  ${source}` : tail;
}

function progressMetrics(state) {
    const percent = Math.round(state.overallPercent());
    const parts = [];
    if (state.total > 0) {
        parts.push(`${state.completed} / ${state.total} 个文件`);
    }
    if (state.totalBytes > 0) {
        parts.push(`${fmtBytes(state.bytes)} / ${fmtBytes(state.totalBytes)}`);
    } else if (state.bytes > 0) {
        parts.push(fmtBytes(state.bytes));
    }
    if (state.speed > 0) {
        parts.push(`${fmtBytes(state.speed)}/s`);
    }
    const eta = state.eta();
    if (eta > 0 && state.speed > 0) {
        parts.push(`剩余 ${fmtDuration(eta)}`);
    }
    if (state.busy && state.elapsed() > 1) {
        parts.push(`已用 ${fmtDuration(state.elapsed())}`);
    }
    return {
        percent,
        meta: parts.join(' · '),
        file: shortFile(state.file),
    };
}

let ctx = null;
let versions = [];
let latestRelease = '';
let latestSnapshot = '';
let versionsLoading = false;
let versionsError = '';
let manifestCacheTime = 0;
let selectedVersion = '';
let versionType = 'release';
let versionSearch = '';
let nameTouched = false;
let installNameValue = '';
let unsubscribeDownload = null;

let selectedLoader = null;
let loaderVersions = [];
let loaderVersionValue = '';
let loaderLoading = false;
let loaderError = '';
let loaderSeq = 0;
let loaderCards = [];

let installNameInput = null;
let selectedVersionLabel = null;
let sourceSelect = null;
let versionListEl = null;
let versionCountEl = null;
let versionHintEl = null;
let versionRefreshButton = null;
let versionTypeButtons = [];
let startButton = null;
let progressCard = null;
let progressStage = null;
let progressMessage = null;
let progressPercentValue = null;
let progressFill = null;
let progressMeta = null;
let progressFile = null;
let progressCancel = null;
let progressActions = null;
let progressSteps = [];

let actionIdle = null;
let actionActive = null;
let actionStage = null;
let actionPercent = null;
let actionFill = null;
let actionCancel = null;
let actionDoneButtons = null;

function downloadSourceLabel(value) {
    return SOURCE_OPTIONS.find(([key]) => key === value)?.[1] || SOURCE_OPTIONS[0][1];
}

function loaderInfo(key) {
    return LOADERS.find((item) => item.key === key) || null;
}

function defaultInstallName() {
    if (!selectedVersion) return '';
    const loader = loaderInfo(selectedLoader);
    return loader ? `${selectedVersion}-${loader.name}` : selectedVersion;
}

function syncDefaultInstallName() {
    if (nameTouched || !installNameInput) return;
    installNameValue = defaultInstallName();
    installNameInput.value = installNameValue;
}

function navItem(key, active) {
    const section = SECTIONS[key];
    const button = el('button', {
        className: 'download-nav-item' + (key === active ? ' active' : ''),
        type: 'button',
        onclick: () => ctx.navigate('#/download/' + key),
    });
    button.append(
        el('span', { className: 'download-nav-icon ' + key, text: section.icon }),
        el('span', { className: 'download-nav-label', text: section.label }),
    );
    if (key === active) button.append(el('span', { className: 'download-nav-dot' }));
    return button;
}

function buildNav(active) {
    const nav = el('div', { className: 'download-nav' });
    nav.append(navItem('install', active));
    nav.append(el('div', { className: 'download-nav-section', text: '社区资源' }));
    for (const key of COMMUNITY_ORDER) nav.append(navItem(key, active));
    return nav;
}

function cardHead(title, extra = null) {
    const head = el('div', { className: 'card-head' });
    head.append(el('h2', { text: title }));
    if (extra) head.append(extra);
    return head;
}

function formatDate(text) {
    return String(text || '').slice(0, 10);
}

function versionBucket(type) {
    if (type === 'release') return 'release';
    if (type === 'snapshot') return 'snapshot';
    return 'old';
}

function installSettingsCard() {
    const card = el('div', { className: 'card install-name-card' });
    const logo = el('div', { className: 'install-logo', text: 'C' });
    const fields = el('div', { className: 'install-name-fields' });

    installNameInput = el('input', {
        type: 'text',
        placeholder: '留空则使用版本名',
        spellcheck: 'false',
        value: installNameValue,
    });
    installNameInput.addEventListener('input', () => {
        installNameValue = installNameInput.value;
        nameTouched = installNameValue.trim().length > 0;
    });

    const nameField = el('label', { className: 'install-field' });
    nameField.append(el('span', { text: '安装名称' }), installNameInput);

    const versionField = el('label', { className: 'install-field' });
    versionField.append(el('span', { text: 'Minecraft 版本' }));
    const selectedWrap = el('div', { className: 'install-selected' });
    selectedVersionLabel = el('span', {
        className: 'install-selected-id',
        text: '正在获取版本列表…',
    });
    selectedWrap.append(selectedVersionLabel);
    versionField.append(selectedWrap);

    sourceSelect = el('select');
    for (const [value, label] of SOURCE_OPTIONS) {
        sourceSelect.append(el('option', { value, text: label }));
    }
    sourceSelect.value = store.settings.downloadSource;
    sourceSelect.addEventListener('change', () => {
        store.settings.downloadSource = sourceSelect.value;
        store.save();
        loadVersions(true);
    });

    const sourceField = el('label', { className: 'install-field install-field-wide' });
    sourceField.append(el('span', { text: '下载源' }), sourceSelect);

    fields.append(nameField, versionField, sourceField);
    card.append(
        logo,
        fields,
        el('span', { className: 'badge soon install-stage', text: 'M3' }),
    );
    return card;
}

function buildVersionCard() {
    const card = el('div', { className: 'card version-card' });

    versionCountEl = el('span', { className: 'version-count', text: '—' });
    versionRefreshButton = el('button', {
        className: 'btn ghost small',
        type: 'button',
        text: '刷新',
        onclick: () => loadVersions(true),
    });
    const headExtra = el('div', { className: 'version-head-extra' }, [
        versionCountEl,
        versionRefreshButton,
    ]);
    card.append(cardHead('Minecraft 版本', headExtra));

    const search = el('input', {
        className: 'version-search',
        type: 'search',
        placeholder: '搜索版本号，例如 1.20.1',
        spellcheck: 'false',
        value: versionSearch,
    });
    search.addEventListener('input', () => {
        versionSearch = search.value;
        renderVersionList();
    });

    const tabs = el('div', { className: 'version-type-tabs' });
    versionTypeButtons = [];
    for (const [value, label] of VERSION_TYPE_TABS) {
        const button = el('button', {
            className: 'install-type' + (versionType === value ? ' active' : ''),
            type: 'button',
            text: label,
            onclick: () => {
                versionType = value;
                for (const item of versionTypeButtons) {
                    item.button.classList.toggle('active', item.value === value);
                }
                renderVersionList();
            },
        });
        versionTypeButtons.push({ value, button });
        tabs.append(button);
    }

    const filters = el('div', { className: 'version-filters' }, [search, tabs]);
    versionListEl = el('div', { className: 'version-list' });
    versionHintEl = el('div', { className: 'version-hint muted' });
    card.append(filters, versionListEl, versionHintEl);
    return card;
}

function filteredVersions() {
    const keyword = versionSearch.trim().toLowerCase();
    return versions.filter((version) => {
        if (versionBucket(version.type) !== versionType) return false;
        return !keyword || version.id.toLowerCase().includes(keyword);
    });
}

function selectVersion(id) {
    selectedVersion = id;
    if (!nameTouched || !installNameValue.trim()) {
        nameTouched = false;
        syncDefaultInstallName();
    }
    updateSelectedLabel();
    updateVersionSelection();
    if (selectedLoader) loadLoaderVersions();
    updateStartButton();
}

function updateVersionSelection() {
    if (!versionListEl) return;
    for (const row of versionListEl.querySelectorAll('.version-row[data-id]')) {
        const active = row.dataset.id === selectedVersion;
        row.classList.toggle('selected', active);
        const check = row.querySelector('.version-check');
        if (check) {
            check.classList.toggle('active', active);
            check.textContent = active ? '✓' : '';
        }
    }
}

function updateSelectedLabel() {
    if (!selectedVersionLabel) return;
    selectedVersionLabel.textContent = selectedVersion || '尚未选择版本';
    selectedVersionLabel.classList.toggle('muted', !selectedVersion);
}

function renderVersionList() {
    if (!versionListEl) return;
    clear(versionListEl);

    if (versionsLoading && !versions.length) {
        versionCountEl.textContent = '正在获取…';
        for (let i = 0; i < 5; i += 1) {
            versionListEl.append(el('div', { className: 'version-row skeleton-row' }));
        }
        return;
    }

    if (versionsError && !versions.length) {
        versionCountEl.textContent = '获取失败';
        const alert = el('div', { className: 'alert', text: versionsError });
        const retry = el('button', {
            className: 'btn ghost small',
            type: 'button',
            text: '重试',
            onclick: () => loadVersions(true),
        });
        versionListEl.append(alert, retry);
        return;
    }

    const list = filteredVersions();
    versionCountEl.textContent = `${list.length} 个版本`;

    if (!list.length) {
        versionListEl.append(el('div', {
            className: 'empty',
            text: versions.length ? '没有匹配的版本' : '版本列表为空',
        }));
        versionHintEl.textContent = '';
        return;
    }

    const shown = list.slice(0, MAX_RENDERED_VERSIONS);
    for (const version of shown) {
        const row = el('button', {
            className: 'version-row' + (version.id === selectedVersion ? ' selected' : ''),
            type: 'button',
            onclick: () => selectVersion(version.id),
        });
        row.dataset.id = version.id;

        const main = el('span', { className: 'version-main' });
        main.append(el('span', { className: 'version-id', text: version.id }));
        main.append(el('span', {
            className: 'badge ' + badgeClass(version.type),
            text: typeLabel(version.type),
        }));
        if (version.id === latestRelease) {
            main.append(el('span', { className: 'badge latest', text: '最新正式版' }));
        } else if (version.id === latestSnapshot) {
            main.append(el('span', { className: 'badge latest', text: '最新快照' }));
        }

        const side = el('span', { className: 'version-side' });
        side.append(el('span', { className: 'version-date', text: formatDate(version.release_time) }));
        side.append(el('span', {
            className: 'version-check' + (version.id === selectedVersion ? ' active' : ''),
            text: version.id === selectedVersion ? '✓' : '',
        }));

        row.append(main, side);
        versionListEl.append(row);
    }

    versionHintEl.textContent = list.length > shown.length
        ? `仅显示前 ${shown.length} 个版本，使用搜索缩小范围。`
        : '';
}

async function loadVersions(force = false) {
    if (versionsLoading) return;
    if (!force && versions.length && Date.now() - manifestCacheTime < 5 * 60 * 1000) {
        renderVersionList();
        return;
    }

    versionsLoading = true;
    versionsError = '';
    if (versionRefreshButton) versionRefreshButton.disabled = true;
    if (!versions.length) {
        selectedVersionLabel.textContent = '正在获取版本列表…';
        selectedVersionLabel.classList.add('muted');
    }
    renderVersionList();
    setStatus('正在获取 Minecraft 版本列表…');

    try {
        const result = await callDownloadVersions(force);
        versions = Array.isArray(result.versions) ? result.versions : [];
        latestRelease = result.latest_release || '';
        latestSnapshot = result.latest_snapshot || '';
        manifestCacheTime = Date.now();
        versionsError = '';

        if (!selectedVersion) {
            const preferred = versions.find((version) => version.id === latestRelease) ||
                versions.find((version) => version.type === 'release') ||
                versions[0];
            if (preferred) {
                selectedVersion = preferred.id;
                if (!nameTouched) syncDefaultInstallName();
            }
        }
        updateSelectedLabel();
        versionType = versions.some((version) => versionBucket(version.type) === versionType)
            ? versionType
            : 'release';
        for (const item of versionTypeButtons) {
            item.button.classList.toggle('active', item.value === versionType);
        }
        renderVersionList();
        updateStartButton();
        if (selectedLoader && selectedVersion) loadLoaderVersions();
        setStatus(`已加载 ${versions.length} 个 Minecraft 版本`);
    } catch (error) {
        versionsError = formatError(error);
        if (!versions.length) {
            selectedVersionLabel.textContent = '版本列表获取失败';
            selectedVersionLabel.classList.add('muted');
        }
        renderVersionList();
        setStatus('版本列表获取失败：' + versionsError.split('\n')[0]);
    } finally {
        versionsLoading = false;
        if (versionRefreshButton) versionRefreshButton.disabled = false;
        renderVersionList();
    }
}

function callDownloadVersions(force) {
    return call('download.versions', {
        source: store.settings.downloadSource,
        force,
    });
}

function updateLoaderSelection() {
    for (const item of loaderCards) {
        const selected = item.loader.key === selectedLoader;
        item.card.classList.toggle('selected', selected);
        item.card.classList.toggle('open', selected);
        item.body.classList.toggle('hidden', !selected);
        if (item.badge) {
            item.badge.textContent = selected ? '已选择' : item.loader.stage;
            item.badge.className = 'badge ' + (selected ? 'latest' : 'soon');
        }
    }
}

function selectLoader(key) {
    const info = loaderInfo(key);
    if (!info?.available) return;
    selectedLoader = key;
    loaderVersions = [];
    loaderVersionValue = '';
    loaderError = '';
    updateLoaderSelection();
    syncDefaultInstallName();
    updateStartButton();
    if (selectedVersion) {
        loadLoaderVersions();
    } else {
        setStatus('请先选择一个 Minecraft 版本');
    }
}

async function loadLoaderVersions() {
    if (!selectedLoader || !selectedVersion) return;
    const entry = loaderCards.find((item) => item.loader.key === selectedLoader);
    if (!entry) return;

    const token = ++loaderSeq;
    loaderLoading = true;
    loaderError = '';
    loaderVersions = [];
    loaderVersionValue = '';
    entry.select.disabled = true;
    clear(entry.select);
    entry.select.append(el('option', {
        text: '正在获取版本列表…',
        disabled: true,
        selected: true,
    }));
    entry.errorNode.classList.add('hidden');
    entry.status.textContent =
        `正在获取 ${entry.loader.name} · Minecraft ${selectedVersion} 的版本…`;
    updateStartButton();

    try {
        const result = await call('loader.versions', {
            loader: selectedLoader,
            game_version: selectedVersion,
        });
        if (token !== loaderSeq || selectedLoader !== entry.loader.key) return;
        const list = Array.isArray(result.versions) ? result.versions : [];
        if (!list.length) {
            throw { message: '这个 Minecraft 版本没有可用的加载器版本' };
        }
        loaderVersions = list;
        clear(entry.select);
        for (const version of list) {
            entry.select.append(el('option', {
                value: version.version,
                text: version.version + (version.stable ? ' · 稳定' : ''),
            }));
        }
        entry.select.disabled = false;
        loaderVersionValue = list[0].version;
        entry.select.value = loaderVersionValue;
        entry.status.textContent =
            `${list.length} 个版本 · 默认选择最新的稳定版`;
        updateStartButton();
    } catch (error) {
        if (token !== loaderSeq) return;
        loaderError = formatError(error);
        clear(entry.select);
        entry.select.append(el('option', {
            text: '获取失败',
            disabled: true,
            selected: true,
        }));
        entry.select.disabled = true;
        entry.errorNode.textContent = loaderError;
        entry.errorNode.classList.remove('hidden');
        entry.status.textContent =
            `${entry.loader.name} 版本列表获取失败`;
        updateStartButton();
    } finally {
        if (token === loaderSeq) {
            loaderLoading = false;
            updateStartButton();
        }
    }
}

function updateStartButton() {
    if (!startButton) return;
    const loaderReady = !selectedLoader ||
        (!loaderLoading && Boolean(loaderVersionValue));
    startButton.disabled = !selectedVersion || downloadStore.busy || !loaderReady;
    if (downloadStore.busy) {
        startButton.textContent = '下载中…';
    } else if (selectedLoader) {
        const info = loaderInfo(selectedLoader);
        startButton.textContent = `安装 ${info?.name || '加载器'}`;
    } else {
        startButton.textContent = '开始下载';
    }
}

async function startInstall() {
    if (!selectedVersion || downloadStore.busy) return;
    if (selectedLoader && !loaderVersionValue) {
        setStatus('请先选择加载器版本');
        return;
    }

    const name = (installNameValue || defaultInstallName()).trim();
    const invalid = validateInstanceName(name);
    if (invalid) {
        setStatus(invalid);
        installNameInput.focus();
        return;
    }

    const job = selectedLoader
        ? downloadStore.start('loader', {
            mc_dir: store.mcDir,
            game_version: selectedVersion,
            loader: selectedLoader,
            loader_version: loaderVersionValue,
            name,
            source: store.settings.downloadSource,
        })
        : downloadStore.start('install', {
            mc_dir: store.mcDir,
            version: selectedVersion,
            name,
            source: store.settings.downloadSource,
        });
    requestAnimationFrame(() => {
        progressCard?.scrollIntoView({ behavior: 'smooth', block: 'nearest' });
    });

    try {
        await job;
        await store.refreshInstances();
        setStatus(`安装完成：${name}`);
        updateStartButton();
    } catch (error) {
        setStatus('安装失败：' + formatError(error).split('\n')[0]);
        updateStartButton();
    }
}

function buildActionBar() {
    const bar = el('div', { className: 'download-action-bar' });

    actionIdle = el('div', { className: 'download-action-idle' });
    const actionText = el('div', { className: 'download-action-text' });
    actionText.append(
        el('strong', { text: '开始下载' }),
        el('span', {
            className: 'muted',
            text: '自动校验 SHA-1；官方源速度慢时会自动切换镜像。',
        }),
    );
    startButton = el('button', {
        className: 'btn primary',
        type: 'button',
        text: '开始下载',
        onclick: startInstall,
    });
    startButton.disabled = true;
    actionIdle.append(actionText, startButton);

    actionActive = el('div', { className: 'download-action-active hidden' });
    actionStage = el('span', { className: 'action-stage', text: '准备中' });
    actionPercent = el('span', { className: 'action-percent', text: '0%' });

    const actionBar = el('div', { className: 'progress compact' });
    actionFill = el('div', { className: 'progress-fill' });
    actionBar.append(actionFill);

    actionCancel = el('button', {
        className: 'btn ghost small',
        type: 'button',
        text: '取消',
        onclick: () => downloadStore.cancel(),
    });
    actionDoneButtons = el('div', { className: 'action-buttons hidden' });
    actionDoneButtons.append(
        el('button', {
            className: 'btn ghost small',
            type: 'button',
            text: '继续安装',
            onclick: () => downloadStore.reset(),
        }),
        el('button', {
            className: 'btn ghost small',
            type: 'button',
            text: '查看版本',
            onclick: () => {
                if (downloadStore.instanceName) {
                    ctx.navigate('#/instances/' + encodeURIComponent(downloadStore.instanceName));
                } else {
                    ctx.navigate('#/instances');
                }
            },
        }),
        el('button', {
            className: 'btn primary small',
            type: 'button',
            text: '去启动',
            onclick: () => ctx.navigate('#/launch'),
        }),
    );

    const line = el('div', { className: 'action-progress-line' });
    line.append(actionStage, actionBar, actionPercent, actionCancel, actionDoneButtons);
    actionActive.append(line);
    bar.append(actionIdle, actionActive);
    return bar;
}

function buildProgressCard() {
    progressCard = el('div', { className: 'card download-progress-card hidden' });
    const head = el('div', { className: 'card-head' });
    head.append(el('h2', { text: '下载进度' }));

    const body = el('div', { className: 'download-progress-body' });
    const top = el('div', { className: 'progress-top' });
    const text = el('div', { className: 'progress-head-text' });
    progressStage = el('div', { className: 'progress-stage', text: '准备中' });
    progressMessage = el('div', { className: 'progress-message muted', text: '' });
    text.append(progressStage, progressMessage);

    const percentBox = el('div', { className: 'progress-percent-box' });
    percentBox.append(el('div', { className: 'progress-percent-label', text: '整体进度' }));
    const percentRow = el('div', { className: 'progress-percent' });
    progressPercentValue = el('span', { text: '0' });
    percentRow.append(progressPercentValue, el('span', { className: 'percent-unit', text: '%' }));
    percentBox.append(percentRow);
    top.append(text, percentBox);

    const steps = el('div', { className: 'progress-steps' });
    progressSteps = STAGE_STEPS.map(([key, label]) => {
        const node = el('span', { className: 'progress-step', text: label });
        steps.append(node);
        return { key, node };
    });

    const bar = el('div', { className: 'progress' });
    progressFill = el('div', { className: 'progress-fill' });
    bar.append(progressFill);

    progressMeta = el('div', { className: 'progress-meta muted', text: '' });
    progressFile = el('div', { className: 'progress-file muted', text: '' });

    progressCancel = el('button', {
        className: 'btn ghost small',
        type: 'button',
        text: '取消下载',
        onclick: () => downloadStore.cancel(),
    });

    progressActions = el('div', { className: 'progress-actions hidden' });
    progressActions.append(
        el('button', {
            className: 'btn ghost small',
            type: 'button',
            text: '继续安装',
            onclick: () => downloadStore.reset(),
        }),
        el('button', {
            className: 'btn ghost small',
            type: 'button',
            text: '查看版本',
            onclick: () => {
                if (downloadStore.instanceName) {
                    ctx.navigate('#/instances/' + encodeURIComponent(downloadStore.instanceName));
                } else {
                    ctx.navigate('#/instances');
                }
            },
        }),
        el('button', {
            className: 'btn primary small',
            type: 'button',
            text: '去启动页',
            onclick: () => ctx.navigate('#/launch'),
        }),
    );

    const footer = el('div', { className: 'progress-footer' }, [
        progressCancel,
        progressActions,
    ]);
    body.append(top, steps, bar, progressMeta, progressFile, footer);
    progressCard.append(head, body);
    return progressCard;
}

function renderProgress(state) {
    if (!progressCard) return;

    // 安装页只关心原版安装/修复任务，避免资源下载进度串到安装页。
    if (state.mode && state.mode !== 'install' && state.mode !== 'repair') {
        progressCard.classList.add('hidden');
        actionIdle.classList.remove('hidden');
        actionActive.classList.add('hidden');
        updateStartButton();
        return;
    }

    const active = state.busy || state.done || Boolean(state.error) ||
        Boolean(state.stage);
    progressCard.classList.toggle('hidden', !active);
    actionIdle.classList.toggle('hidden', active);
    actionActive.classList.toggle('hidden', !active);
    if (!active) {
        updateStartButton();
        return;
    }

    const metrics = progressMetrics(state);
    const percent = Math.max(0, Math.min(100, metrics.percent));
    const cancelled = Boolean(state.error) && state.error.includes('取消');
    const stage = state.error
        ? (cancelled ? '已取消' : '下载失败')
        : `${stageLabel(state.stage)}${state.mode === 'repair' ? ' · 修复' : ''}`;
    const message = state.error
        ? state.error.split('\n')[0]
        : (state.message || '');

    progressFill.style.width = `${percent}%`;
    progressFill.classList.toggle('error', Boolean(state.error));
    progressFill.classList.toggle('done', state.done);
    progressFill.classList.toggle('indeterminate', state.busy && percent <= 0);
    progressPercentValue.textContent = String(percent);
    progressStage.textContent = stage;
    progressMessage.textContent = message;
    progressMeta.textContent = state.done ? '全部文件已就绪' : metrics.meta;
    progressFile.textContent = metrics.file;

    progressCancel.classList.toggle('hidden', !state.busy);
    progressActions.classList.toggle('hidden', state.busy);
    progressCard.classList.toggle('done', state.done);
    progressCard.classList.toggle('error', Boolean(state.error));

    let stageIndex = STAGE_STEPS.findIndex(([key]) => key === state.stage);
    if (stageIndex < 0 && state.done) stageIndex = STAGE_STEPS.length - 1;
    for (let i = 0; i < progressSteps.length; i += 1) {
        const step = progressSteps[i];
        step.node.classList.toggle('done', state.done || (stageIndex >= 0 && i < stageIndex));
        step.node.classList.toggle('active',
            !state.done && !state.error && stageIndex >= 0 && i === stageIndex);
        step.node.classList.toggle('error', Boolean(state.error) && stageIndex === i);
    }

    actionFill.style.width = `${percent}%`;
    actionFill.classList.toggle('error', Boolean(state.error));
    actionFill.classList.toggle('done', state.done);
    actionFill.classList.toggle('indeterminate', state.busy && percent <= 0);
    actionStage.textContent = stage;
    actionStage.title = [metrics.meta, metrics.file].filter(Boolean).join('\n');
    actionPercent.textContent = `${percent}%`;
    actionCancel.classList.toggle('hidden', !state.busy);
    actionDoneButtons.classList.toggle('hidden', state.busy);

    if (state.error) {
        setStatus((cancelled ? '已取消下载：' : '下载失败：') + message);
    } else if (state.done) {
        setStatus(`安装完成：${state.instanceName || state.version}`);
    } else if (state.busy) {
        setStatus(`${stage} · ${percent}%${state.speed > 0
            ? ' · ' + fmtBytes(state.speed) + '/s'
            : ''}`);
    }

    updateStartButton();
}

function installPage() {
    const vanilla = store.instances.find((item) => item.loader === 'vanilla');
    const note = el('div', { className: 'card install-note' });
    note.append(
        cardHead('说明'),
        el('p', {
            className: 'muted',
            text: '下载会写入 versions、libraries 和 assets，并逐文件校验 SHA-1。中断后可以重新开始或对已有版本使用“修复”。',
        }),
    );
    if (vanilla) {
        note.append(el('p', {
            className: 'muted',
            text: `当前游戏目录里已经有原版 ${vanilla.id}，可以继续安装其它版本。`,
        }));
    }
    note.append(el('div', { className: 'install-note-actions' }, [
        el('button', {
            className: 'btn ghost',
            type: 'button',
            text: '前往版本管理',
            onclick: () => ctx.navigate('#/instances'),
        }),
    ]));

    loaderCards = [];
    if (selectedLoader && !loaderInfo(selectedLoader)?.available) {
        selectedLoader = null;
        loaderVersions = [];
        loaderVersionValue = '';
    }

    const page = [
        installSettingsCard(),
        buildProgressCard(),
        buildVersionCard(),
        el('div', { className: 'install-section-label', text: '模组加载器（M4）' }),
        el('div', { className: 'loader-grid' }, LOADERS.map(loaderCard)),
        note,
        buildActionBar(),
    ];

    updateLoaderSelection();
    updateStartButton();
    if (selectedLoader && selectedVersion) loadLoaderVersions();

    if (unsubscribeDownload) unsubscribeDownload();
    unsubscribeDownload = downloadStore.subscribe(renderProgress);

    return page;
}

function loaderCard(loader) {
    const card = el('div', {
        className: 'card loader-card' + (loader.available ? '' : ' disabled'),
    });
    const head = el('button', { className: 'loader-card-head', type: 'button' });

    const logo = el('span', {
        className: 'loader-logo ' + loader.key,
        text: loader.letter,
    });
    const text = el('span', { className: 'loader-card-text' });
    text.append(
        el('span', { className: 'loader-card-name', text: loader.name }),
        el('span', { className: 'loader-card-desc', text: loader.desc }),
    );
    const badge = el('span', {
        className: 'badge ' + (loader.available ? 'latest' : 'soon'),
        text: loader.stage,
    });
    const meta = el('span', { className: 'loader-card-meta' });
    meta.append(badge, el('span', { className: 'loader-chevron', text: '›' }));

    head.append(logo, text, meta);
    const body = el('div', { className: 'loader-body hidden' });

    let select = null;
    let status = null;
    let errorNode = null;
    if (loader.available) {
        select = el('select', { className: 'loader-version-select' });
        select.append(el('option', {
            text: '先选择 Minecraft 版本',
            value: '',
            disabled: true,
            selected: true,
        }));
        select.addEventListener('change', () => {
            loaderVersionValue = select.value;
            updateStartButton();
        });
        const refresh = el('button', {
            className: 'btn ghost small',
            type: 'button',
            text: '刷新',
            onclick: () => loadLoaderVersions(),
        });
        status = el('div', {
            className: 'loader-status muted',
            text: '选择这个加载器后，会按当前 Minecraft 版本获取可用版本。',
        });
        errorNode = el('div', { className: 'loader-error hidden' });
        body.append(
            el('div', { className: 'loader-version-row' }, [select, refresh]),
            status,
            errorNode,
            el('div', {
                className: 'loader-note muted',
                text: '安装时会先确保原版文件完整；Forge / NeoForge 会调用官方安装器。',
            }),
        );
    } else {
        body.append(el('div', {
            className: 'loader-empty',
            text: `${loader.name} 会在后续里程碑接入，目前可先安装 Forge、NeoForge、Fabric 或 Quilt。`,
        }));
    }

    head.addEventListener('click', () => {
        if (!loader.available) {
            const open = body.classList.toggle('hidden') === false;
            card.classList.toggle('open', open);
            return;
        }
        selectLoader(loader.key);
    });
    card.append(head, body);
    loaderCards.push({ loader, card, body, select, status, errorNode, badge });
    return card;
}

// ==========================================
// Modrinth 资源搜索与下载
// ==========================================

let resourceState = null;

const RESOURCE_SORTS = [
    ['relevance', '相关性'],
    ['downloads', '下载量'],
    ['follows', '关注数'],
    ['newest', '最新发布'],
    ['updated', '最近更新'],
];

const RESOURCE_FOLDERS = {
    mod: 'mods',
    modpack: 'modpacks',
    resourcepack: 'resourcepacks',
    shader: 'shaderpacks',
    datapack: 'datapacks',
};

function resourceType(section) {
    switch (section.itemName) {
        case '整合包': return 'modpack';
        case '数据包': return 'datapack';
        case '资源包': return 'resourcepack';
        case '光影包': return 'shader';
        default: return 'mod';
    }
}

function formatCount(value) {
    const number = Math.max(0, Number(value) || 0);
    if (number >= 100000000) return `${(number / 100000000).toFixed(1)} 亿`;
    if (number >= 10000) return `${(number / 10000).toFixed(1)} 万`;
    return number.toLocaleString('zh-CN');
}

function instanceGameVersion(instance) {
    if (!instance) return '';
    return instance.inherits_from || instance.id || '';
}

function defaultResourceTarget() {
    if (store.selected &&
        store.instances.some((item) => item.id === store.selected)) {
        return store.selected;
    }
    return store.instances[0]?.id || '';
}

function resourceInstance() {
    const id = resourceState?.targetId;
    if (!id) return null;
    return store.instances.find((item) => item.id === id) || null;
}

function updateResourceTargetHint() {
    const state = resourceState;
    const hint = state?.elements?.hint;
    if (!hint) return;

    const instance = resourceInstance();
    let text = '';
    let warn = false;

    if (state.type === 'mod') {
        if (!instance) {
            text = '请先创建一个实例；Mod 需要放进实例的 mods 文件夹。';
            warn = true;
        } else if (instance.loader === 'vanilla') {
            text = `${instance.id} 是原版实例，Mod 不会生效。请先安装 Forge / NeoForge / Fabric / Quilt。`;
            warn = true;
        } else {
            text = `将安装到 ${instance.id} 的 mods 文件夹（${loaderLabel(instance.loader)} · ${instanceGameVersion(instance)}）。`;
        }
    } else if (state.type === 'modpack') {
        text = '点击版本旁的「安装」会自动下载整合包、安装原版和依赖加载器并创建独立实例；也可以导入本地 .mrpack，或直接把整合包拖进窗口。';
    } else {
        const folder = RESOURCE_FOLDERS[state.type] || state.type;
        text = instance
            ? `将安装到 ${instance.id} 的 ${folder} 文件夹。`
            : `将安装到全局 .minecraft/${folder} 文件夹。`;
        if (state.type === 'datapack') {
            text += ' 数据包需要放进具体存档的 datapacks 文件夹才会生效。';
            warn = true;
        }
    }

    hint.textContent = text;
    hint.classList.toggle('warn', warn);
}

function fillResourceTargets() {
    const state = resourceState;
    const select = state?.elements?.target;
    if (!select) return;

    const previous = state.targetId || select.value;
    clear(select);

    if (state.type !== 'mod') {
        select.append(el('option', {
            value: '',
            text: '不指定实例（全局目录）',
        }));
    }
    if (state.type === 'mod' && !store.instances.length) {
        select.append(el('option', {
            value: '',
            text: '没有可用实例',
            disabled: true,
        }));
    }
    for (const instance of store.instances) {
        select.append(el('option', {
            value: instance.id,
            text: `${instance.id}（${loaderLabel(instance.loader)}）`,
        }));
    }

    const exists = [...select.options].some((option) => option.value === previous);
    const fallback = state.type === 'mod' ? defaultResourceTarget() : '';
    select.value = exists ? previous : fallback;
    state.targetId = select.value;
    updateResourceTargetHint();
}

function searchCard(section) {
    const state = resourceState;
    const card = el('div', { className: 'card resource-search-card' });
    card.append(cardHead(
        `搜索${section.itemName}`,
        el('span', { className: 'badge latest', text: 'Modrinth' }),
    ));

    const form = el('div', { className: 'resource-form' });
    const field = (label, input, className = '') => {
        const node = el('label', { className: 'resource-field ' + className });
        node.append(el('span', { text: label }), input);
        return node;
    };

    const nameInput = el('input', {
        type: 'search',
        placeholder: `输入${section.itemName}名称，留空查看热门`,
        spellcheck: 'false',
    });
    nameInput.value = state.query;

    const gameInput = el('input', {
        type: 'text',
        placeholder: '例如 1.21.1',
        spellcheck: 'false',
    });
    gameInput.value = state.game;

    const loaderSelect = el('select');
    loaderSelect.append(el('option', { value: '', text: '任意 Mod 加载器' }));
    for (const key of ['forge', 'neoforge', 'fabric', 'quilt']) {
        loaderSelect.append(el('option', { value: key, text: loaderLabel(key) }));
    }
    loaderSelect.value = state.loader || '';

    const sortSelect = el('select');
    for (const [value, label] of RESOURCE_SORTS) {
        sortSelect.append(el('option', { value, text: label }));
    }
    sortSelect.value = state.index;

    const targetSelect = el('select');
    targetSelect.addEventListener('change', () => {
        state.targetId = targetSelect.value;
        updateResourceTargetHint();
    });

    const hint = el('div', { className: 'resource-hint muted' });

    form.append(field('关键词', nameInput, 'name'), field('游戏版本', gameInput));
    if (section.showLoader) form.append(field('加载器', loaderSelect));
    form.append(field('排序', sortSelect));
    if (state.type !== 'modpack') {
        form.append(field('安装到', targetSelect, 'target'));
    }

    Object.assign(state.elements, {
        name: nameInput,
        game: gameInput,
        loader: loaderSelect,
        sort: sortSelect,
        target: targetSelect,
        hint,
    });

    const actions = el('div', { className: 'resource-actions' });
    const searchBtn = el('button', {
        className: 'btn primary',
        type: 'button',
        text: '搜索',
    });
    const resetBtn = el('button', {
        className: 'btn ghost',
        type: 'button',
        text: '重置条件',
    });

    const applySearch = () => {
        state.query = nameInput.value.trim();
        state.game = gameInput.value.trim();
        state.loader = section.showLoader ? loaderSelect.value : '';
        state.index = sortSelect.value;
        loadResourcePage(true);
    };

    searchBtn.addEventListener('click', applySearch);
    for (const input of [nameInput, gameInput]) {
        input.addEventListener('keydown', (event) => {
            if (event.key === 'Enter') {
                event.preventDefault();
                applySearch();
            }
        });
    }
    resetBtn.addEventListener('click', () => {
        nameInput.value = '';
        gameInput.value = '';
        loaderSelect.value = '';
        sortSelect.value = 'relevance';
        state.query = '';
        state.game = '';
        state.loader = '';
        state.index = 'relevance';
        state.targetId = state.type === 'mod' ? defaultResourceTarget() : '';
        targetSelect.value = state.targetId;
        updateResourceTargetHint();
        loadResourcePage(true);
    });
    actions.append(searchBtn, resetBtn);
    if (state.type === 'modpack') {
        actions.append(el('button', {
            className: 'btn ghost',
            type: 'button',
            text: '导入本地整合包',
            onclick: importLocalModpack,
        }));
    }

    card.append(form, hint, actions);
    fillResourceTargets();
    return card;
}

function resourceIcon(project) {
    const wrap = el('div', { className: 'resource-icon' });
    const letter = (project.title || '?').slice(0, 1).toUpperCase();
    if (!project.icon_url) {
        wrap.append(el('span', { text: letter }));
        return wrap;
    }
    const img = el('img', {
        src: project.icon_url,
        alt: '',
        loading: 'lazy',
        referrerpolicy: 'no-referrer',
    });
    img.addEventListener('error', () => {
        img.remove();
        wrap.append(el('span', { text: letter }));
    });
    wrap.append(img);
    return wrap;
}

function resourceProjectCard(project) {
    const card = el('article', { className: 'resource-result' });

    const body = el('div', { className: 'resource-result-body' });
    const titleRow = el('div', { className: 'resource-result-title' });
    titleRow.append(el('h3', { text: project.title || '未命名项目' }));
    if (project.author) {
        titleRow.append(el('span', {
            className: 'resource-author muted',
            text: `by ${project.author}`,
        }));
    }

    const desc = el('p', {
        className: 'resource-desc',
        text: project.description || '没有描述',
        title: project.description || '',
    });

    const meta = el('div', { className: 'resource-meta muted' });
    meta.append(
        el('span', { text: `↓ ${formatCount(project.downloads)}` }),
        el('span', { text: `♥ ${formatCount(project.follows)}` }),
    );
    if (project.date_modified) {
        meta.append(el('span', {
            text: `更新于 ${String(project.date_modified).slice(0, 10)}`,
        }));
    }

    const tags = el('div', { className: 'resource-tags' });
    for (const game of (project.game_versions || []).slice(-3)) {
        tags.append(el('span', { className: 'resource-tag', text: game }));
    }
    for (const category of (project.categories || []).slice(0, 3)) {
        tags.append(el('span', {
            className: 'resource-tag soft',
            text: category,
        }));
    }

    const actions = el('div', { className: 'resource-result-actions' });
    const toggle = el('button', {
        className: 'btn primary small',
        type: 'button',
        text: '选择版本',
    });
    actions.append(toggle);

    const versions = el('div', { className: 'resource-versions hidden' });
    toggle.addEventListener('click', () => {
        toggleResourceVersions(project, versions, toggle);
    });

    body.append(titleRow, desc, meta, tags);
    card.append(resourceIcon(project), body, actions, versions);
    return card;
}

function resourceVersionRow(project, version) {
    const row = el('div', { className: 'resource-version' });
    const main = el('div', { className: 'resource-version-main' });

    const title = el('div', { className: 'resource-version-title' });
    title.append(el('span', {
        text: version.version_number || version.name || version.id,
    }));
    if (version.version_type) {
        title.append(el('span', {
            className: 'badge ' + badgeClass(version.version_type),
            text: typeLabel(version.version_type),
        }));
    }

    const meta = el('div', { className: 'resource-version-meta muted' });
    const parts = [];
    if (version.game_versions?.length) {
        parts.push(version.game_versions.slice(0, 3).join(' / '));
    }
    if (version.loaders?.length) {
        parts.push(version.loaders.map(loaderLabel).join(' / '));
    }
    if (version.date_published) {
        parts.push(String(version.date_published).slice(0, 10));
    }
    meta.textContent = parts.join(' · ');

    main.append(title, meta);

    const file = version.file || {};
    const isModpack = resourceState?.type === 'modpack';
    const download = el('button', {
        className: 'btn primary small',
        type: 'button',
        text: isModpack ? '安装' : '下载',
    });
    if (file.size) download.title = fmtBytes(file.size);
    download.addEventListener('click', () => {
        if (isModpack) {
            installModpackVersion(project, version, download);
        } else {
            downloadResourceVersion(project, version, download);
        }
    });

    row.append(main, download);
    return row;
}

async function toggleResourceVersions(project, container, button) {
    const state = resourceState;
    if (!state) return;

    if (!container.classList.contains('hidden')) {
        container.classList.add('hidden');
        button.textContent = '选择版本';
        return;
    }

    container.classList.remove('hidden');
    button.textContent = '收起版本';
    if (container.dataset.loaded === '1') return;
    container.dataset.loaded = '1';

    clear(container);
    container.append(el('div', {
        className: 'resource-version-empty muted',
        text: '正在获取版本列表…',
    }));

    try {
        const result = await call('modrinth.versions', {
            project_id: project.id,
            game_version: state.game,
            loader: state.type === 'mod' ? state.loader : '',
        });
        if (resourceState !== state) return;

        const versions = Array.isArray(result.versions) ? result.versions : [];
        clear(container);
        if (!versions.length) {
            container.append(el('div', {
                className: 'resource-version-empty muted',
                text: '这个筛选条件下没有可用版本。换个游戏版本或加载器再试试。',
            }));
            return;
        }
        for (const version of versions.slice(0, 30)) {
            container.append(resourceVersionRow(project, version));
        }
        if (versions.length > 30) {
            container.append(el('div', {
                className: 'resource-version-empty muted',
                text: `仅显示最近 30 个版本，共 ${versions.length} 个。`,
            }));
        }
    } catch (error) {
        container.dataset.loaded = '0';
        clear(container);
        container.append(el('div', {
            className: 'resource-version-empty error',
            text: formatError(error),
        }));
    }
}

async function downloadResourceVersion(project, version, button) {
    const state = resourceState;
    if (!state) return;
    if (downloadStore.busy) {
        setStatus('已有下载任务正在进行');
        return;
    }
    if (state.type === 'mod' && !state.targetId) {
        setStatus('请先选择一个实例，再下载 Mod');
        return;
    }

    const file = version.file || {};
    button.disabled = true;
    button.textContent = '下载中…';

    try {
        const result = await downloadStore.start('resource', {
            mc_dir: store.mcDir,
            instance_id: state.type === 'modpack' ? '' : state.targetId,
            kind: state.type,
            version_id: version.id,
            project_title: project.title,
            filename: file.filename || '',
        });
        if (resourceState !== state) return;

        button.textContent = '已下载';
        const name = result?.filename || file.filename || project.title;
        if (state.type === 'modpack') {
            setStatus(`整合包已下载：${name}（.minecraft/modpacks）`);
        } else {
            setStatus(`已下载 ${name}`);
        }
    } catch (error) {
        button.disabled = false;
        button.textContent = '重试';
        setStatus('下载失败：' + formatError(error).split('\n')[0]);
    }
}

async function installModpackVersion(project, version, button) {
    const state = resourceState;
    if (!state) return;
    if (downloadStore.busy) {
        setStatus('已有下载任务正在进行');
        return;
    }

    button.disabled = true;
    button.textContent = '安装中…';

    try {
        const result = await downloadStore.start('modpack', {
            mc_dir: store.mcDir,
            version_id: version.id,
            project_title: project.title,
            instance_name: project.title,
        });
        if (resourceState !== state) return;

        button.textContent = '已安装';
        const installed = result?.instance_id || project.title;
        setStatus(`整合包已安装：${installed}`);
        try {
            await store.refreshInstances();
        } catch (_) {
            // 实例详情页会自行刷新。
        }
        if (result?.instance_id) {
            ctx.navigate('#/instances/' + encodeURIComponent(result.instance_id));
        }
    } catch (error) {
        button.disabled = false;
        button.textContent = '重试';
        setStatus('安装失败：' + formatError(error).split('\n')[0]);
    }
}

async function importLocalModpack() {
    if (downloadStore.busy) {
        setStatus('已有下载任务正在进行');
        return;
    }

    setStatus('请选择 .mrpack 整合包…');
    try {
        const picked = await call('dialog.open_modpack');
        if (!picked?.path) {
            setStatus('已取消导入');
            return;
        }

        const result = await downloadStore.start('modpack', {
            mc_dir: store.mcDir,
            path: picked.path,
        });
        const installed = result?.instance_id || result?.name || '整合包';
        setStatus(`整合包已安装：${installed}`);
        try {
            await store.refreshInstances();
        } catch (_) {
            // 实例详情页会自行刷新。
        }
        if (result?.instance_id) {
            ctx.navigate('#/instances/' + encodeURIComponent(result.instance_id));
        }
    } catch (error) {
        setStatus('导入失败：' + formatError(error).split('\n')[0]);
    }
}

function resultsCard() {
    const state = resourceState;
    const card = el('div', { className: 'card resource-results-card' });
    const count = el('span', { className: 'resource-count', text: '尚未搜索' });
    card.append(cardHead('搜索结果', count));

    const list = el('div', { className: 'resource-results' });
    state.elements.count = count;
    state.elements.list = list;
    card.append(list);
    renderResourceResults();
    return card;
}

function renderResourceResults() {
    const state = resourceState;
    const list = state?.elements?.list;
    if (!list) return;

    clear(list);
    if (state.elements.count) {
        state.elements.count.textContent = state.loading
            ? '搜索中…'
            : `${state.total} 个结果`;
    }

    if (state.loading) {
        const skeleton = el('div', { className: 'resource-loading' });
        for (let i = 0; i < 4; i += 1) {
            skeleton.append(el('div', { className: 'resource-skeleton' }));
        }
        list.append(skeleton);
        return;
    }

    if (state.error) {
        const empty = el('div', { className: 'resource-empty' });
        empty.append(
            el('div', { className: 'resource-empty-icon', text: '!' }),
            el('h3', { text: '搜索失败' }),
            el('p', { text: state.error }),
            el('button', {
                className: 'btn ghost small',
                type: 'button',
                text: '重试',
                onclick: () => loadResourcePage(false),
            }),
        );
        list.append(empty);
        return;
    }

    if (!state.projects.length) {
        const empty = el('div', { className: 'resource-empty' });
        empty.append(
            el('div', { className: 'resource-empty-icon', text: '⌕' }),
            el('h3', { text: '没有找到结果' }),
            el('p', {
                text: state.query
                    ? '换个关键词，或者调整游戏版本和加载器再试试。'
                    : '暂无可展示的资源，试试输入关键词搜索。',
            }),
        );
        list.append(empty);
        return;
    }

    for (const project of state.projects) {
        list.append(resourceProjectCard(project));
    }
}

function resourceProgressCard() {
    const state = resourceState;
    const card = el('div', {
        className: 'card download-progress-card resource-progress hidden',
    });

    const top = el('div', { className: 'progress-top' });
    const headText = el('div', { className: 'progress-head-text' });
    const stage = el('div', { className: 'progress-stage', text: '资源下载' });
    const message = el('div', { className: 'progress-message', text: '' });
    headText.append(stage, message);

    const percentBox = el('div', { className: 'progress-percent-box' });
    percentBox.append(el('div', {
        className: 'progress-percent-label',
        text: '当前任务',
    }));
    const percent = el('div', { className: 'progress-percent' });
    const percentValue = el('span', { text: '0' });
    percent.append(percentValue, el('span', {
        className: 'percent-unit',
        text: '%',
    }));
    percentBox.append(percent);
    top.append(headText, percentBox);

    const bar = el('div', { className: 'progress' });
    const fill = el('div', { className: 'progress-fill' });
    bar.append(fill);

    const meta = el('div', { className: 'progress-meta muted' });
    const file = el('div', { className: 'progress-file muted' });
    const cancel = el('button', {
        className: 'btn ghost small',
        type: 'button',
        text: '取消下载',
    });
    cancel.addEventListener('click', () => downloadStore.cancel());

    const actions = el('div', { className: 'resource-actions' }, [cancel]);

    card.append(top, bar, meta, file, actions);
    state.elements.progress = {
        card, stage, message, percentValue, fill, meta, file, cancel,
    };
    return card;
}

function renderResourceProgress(dl) {
    const node = resourceState?.elements?.progress;
    if (!node) return;

    const active = (dl.mode === 'resource' || dl.mode === 'modpack') &&
        (dl.busy || dl.done || Boolean(dl.error));
    node.card.classList.toggle('hidden', !active);
    if (!active) return;

    const isModpack = dl.mode === 'modpack';
    const percent = dl.done ? 100 : Math.max(0, Math.min(100, dl.percent()));
    const cancelled = Boolean(dl.error) && dl.error.includes('取消');
    node.percentValue.textContent = String(Math.round(percent));
    node.stage.textContent = dl.error
        ? (cancelled ? '已取消' : (isModpack ? '安装失败' : '下载失败'))
        : (dl.done
            ? (isModpack ? '安装完成' : '下载完成')
            : (isModpack ? '整合包安装' : '资源下载'));
    node.message.textContent = dl.message || '';
    node.fill.style.width = percent + '%';
    node.fill.classList.toggle('error', Boolean(dl.error));
    node.fill.classList.toggle('done', dl.done);
    node.fill.classList.toggle(
        'indeterminate',
        !dl.done && !dl.error && dl.busy &&
        dl.totalBytes <= 0 && dl.total <= 0,
    );

    const parts = [];
    if (dl.totalBytes > 0) {
        parts.push(`${fmtBytes(dl.bytes)} / ${fmtBytes(dl.totalBytes)}`);
        if (dl.speed > 0) parts.push(`${fmtBytes(dl.speed)}/s`);
        if (dl.eta() > 0) parts.push(`剩余 ${fmtDuration(dl.eta())}`);
    } else if (dl.total > 0) {
        parts.push(`${dl.completed} / ${dl.total}`);
    }
    parts.push(dl.done ? '完成' : `已用 ${fmtDuration(dl.elapsed())}`);
    node.meta.textContent = parts.join(' · ');
    node.file.textContent = dl.file || '';
    node.cancel.classList.toggle('hidden', !dl.busy || Boolean(dl.error));
}

function pagerCard() {
    const state = resourceState;
    const card = el('div', { className: 'card resource-pager-card' });
    const pager = el('div', { className: 'resource-pager' });

    const make = (text, action) => {
        const button = el('button', {
            className: 'btn ghost small',
            type: 'button',
            text,
        });
        button.addEventListener('click', action);
        return button;
    };

    const pages = () => Math.max(1, Math.ceil(state.total / state.limit));
    const first = make('«', () => {
        if (state.offset <= 0) return;
        state.offset = 0;
        loadResourcePage(false);
    });
    const prev = make('‹', () => {
        if (state.offset <= 0) return;
        state.offset = Math.max(0, state.offset - state.limit);
        loadResourcePage(false);
    });
    const label = el('span', {
        className: 'resource-page',
        text: '第 1 页',
    });
    const next = make('›', () => {
        if (state.offset + state.limit >= state.total) return;
        state.offset += state.limit;
        loadResourcePage(false);
    });
    const last = make('»', () => {
        const target = (pages() - 1) * state.limit;
        if (state.offset === target) return;
        state.offset = Math.max(0, target);
        loadResourcePage(false);
    });

    pager.append(first, prev, label, next, last);
    card.append(pager);
    state.elements.pager = { first, prev, label, next, last };
    updateResourcePager();
    return card;
}

function updateResourcePager() {
    const state = resourceState;
    const pager = state?.elements?.pager;
    if (!pager) return;

    const totalPages = Math.max(1, Math.ceil(state.total / state.limit));
    const page = Math.min(
        totalPages,
        Math.floor(state.offset / state.limit) + 1,
    );
    pager.label.textContent = `第 ${page} / ${totalPages} 页 · 共 ${state.total} 个`;

    const disabled = state.loading || state.total <= 0;
    pager.first.disabled = disabled || page <= 1;
    pager.prev.disabled = disabled || page <= 1;
    pager.next.disabled = disabled || page >= totalPages;
    pager.last.disabled = disabled || page >= totalPages;
}

async function loadResourcePage(resetOffset) {
    const state = resourceState;
    if (!state) return;

    if (resetOffset) state.offset = 0;
    const token = ++state.seq;
    state.loading = true;
    state.error = '';

    renderResourceResults();
    updateResourcePager();
    setStatus(state.query
        ? `正在搜索「${state.query}」…`
        : '正在获取热门资源…');

    try {
        const index = !state.query && state.index === 'relevance'
            ? 'downloads'
            : state.index;
        const result = await call('modrinth.search', {
            query: state.query,
            type: state.type,
            game_version: state.game,
            loader: state.type === 'mod' ? state.loader : '',
            index,
            offset: state.offset,
            limit: state.limit,
        });
        if (resourceState !== state || token !== state.seq) return;

        state.projects = Array.isArray(result.projects) ? result.projects : [];
        state.total = Number(result.total_hits) || 0;
        state.offset = Number(result.offset) || 0;
        state.loading = false;
        renderResourceResults();
        updateResourcePager();
        setStatus(state.total
            ? `找到 ${state.total} 个结果`
            : '没有找到匹配的资源');
    } catch (error) {
        if (resourceState !== state || token !== state.seq) return;
        state.loading = false;
        state.projects = [];
        state.total = 0;
        state.error = formatError(error);
        renderResourceResults();
        updateResourcePager();
        setStatus('搜索失败：' + state.error.split('\n')[0]);
    }
}

function resourcePage(section) {
    const state = {
        section,
        type: resourceType(section),
        query: '',
        game: '',
        loader: '',
        index: 'relevance',
        offset: 0,
        limit: 20,
        total: 0,
        projects: [],
        targetId: '',
        loading: true,
        error: '',
        seq: 0,
        elements: {},
    };
    resourceState = state;

    const instance = store.selected
        ? store.instances.find((item) => item.id === store.selected)
        : store.instances[0];
    if (instance) {
        state.game = instanceGameVersion(instance);
        if (state.type === 'mod' && instance.loader !== 'vanilla') {
            state.loader = instance.loader;
        }
        state.targetId = instance.id;
    }

    const page = [
        searchCard(section),
        resourceProgressCard(),
        resultsCard(),
        pagerCard(),
    ];

    if (!store.instances.length) {
        store.refreshInstances().then(() => {
            if (resourceState !== state || state.targetId) return;
            const first = store.instances.find((item) => item.id === store.selected)
                || store.instances[0];
            if (first) {
                state.targetId = first.id;
                state.game = instanceGameVersion(first);
                if (state.type === 'mod' && first.loader !== 'vanilla') {
                    state.loader = first.loader;
                }
                if (state.elements.game) state.elements.game.value = state.game;
                if (state.elements.loader) {
                    state.elements.loader.value = state.loader;
                }
            }
            fillResourceTargets();
        }).catch(() => {});
    }

    // 上一个资源下载/整合包安装已结束时清掉旧卡片，避免切换分类后还显示。
    if (!downloadStore.busy &&
        (downloadStore.mode === 'resource' || downloadStore.mode === 'modpack')) {
        downloadStore.reset();
    }
    if (unsubscribeDownload) unsubscribeDownload();
    unsubscribeDownload = downloadStore.subscribe(renderResourceProgress);

    queueMicrotask(() => {
        if (resourceState === state) loadResourcePage(true);
    });

    return page;
}

export function mount(context, params) {
    ctx = context;
    // 切换下载分类时回到顶部，避免沿用上一页的滚动位置。
    ctx.page.scrollTop = 0;

    let key = params?.section || 'install';
    if (key === 'resource') key = 'mod';
    if (!SECTIONS[key]) {
        ctx.navigate('#/download/install');
        return;
    }

    const section = SECTIONS[key];
    setBreadcrumb(['下载', section.label]);
    setSidebar([buildNav(key)]);
    setSidebarFooter([
        el('button', {
            className: 'btn ghost small',
            text: '管理已安装版本',
            type: 'button',
            onclick: () => ctx.navigate('#/instances'),
        }),
    ]);

    if (section.kind === 'install') {
        setTopActions([]);
        setStatus('M3 下载器');
        ctx.page.append(...installPage());
        updateSelectedLabel();
        loadVersions(false);
    } else {
        setTopActions([]);
        setStatus('正在连接 Modrinth…');
        ctx.page.append(...resourcePage(section));
    }
}

export function unmount() {
    if (unsubscribeDownload) unsubscribeDownload();
    unsubscribeDownload = null;
    resourceState = null;
    loaderSeq++;
    loaderCards = [];
    loaderLoading = false;
    ctx = null;
}