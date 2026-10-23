import { call } from '../rpc.js';
import { store } from '../store.js';
import { downloadStore } from '../download-store.js';
import { el, clear, fmtBytes, fmtDuration, formatError } from '../utils.js';
import {
    setStatus,
    setBreadcrumb,
    setTopActions,
    setSidebar,
    confirmDialog,
} from '../ui.js';

const SECTIONS = {
    general: '常规',
    account: '账号',
    java: 'Java',
    logs: '日志',
    about: '关于',
};

const ORDER = ['general', 'account', 'java', 'logs', 'about'];

let ctx = null;
let javaSeq = 0;
let jdkSeq = 0;
let accountSeq = 0;
let accountHolder = null;
let accountInfo = null;
let accountError = '';
let deviceLogin = null;
let devicePollTimer = null;
let devicePollBusy = false;
let unsubscribeDownload = null;
let detectedJavas = [];
let lastReleases = [];
let jdkButtons = [];
let javaActionBusy = false;

let jdkGrid = null;
let javaTableBody = null;
let javaProgressCard = null;
let javaProgressStage = null;
let javaProgressFill = null;
let javaProgressPercent = null;
let javaProgressMeta = null;
let javaProgressFile = null;
let javaProgressCancel = null;

function sectionTabs(active) {
    const wrap = el('div', { className: 'section-tabs' });
    for (const key of ORDER) {
        wrap.append(el('button', {
            className: 'section-tab' + (key === active ? ' active' : ''),
            text: SECTIONS[key],
            type: 'button',
            onclick: () => ctx.navigate('#/settings/' + key),
        }));
    }
    return wrap;
}

function settingRow(label, desc, control) {
    const row = el('div', { className: 'setting-row' });
    const text = el('div');
    text.append(el('div', { className: 'setting-label', text: label }));
    if (desc) text.append(el('div', { className: 'setting-desc', text: desc }));
    const value = el('div', { className: 'setting-control' });
    value.append(control instanceof Node ? control : el('span', { className: 'muted', text: control }));
    row.append(text, value);
    return row;
}

function card(title, ...children) {
    const node = el('div', { className: 'card settings-section' });
    const head = el('div', { className: 'card-head' });
    head.append(el('h2', { text: title }));
    node.append(head, ...children);
    return node;
}

function pathText(text) {
    const node = el('span', {
        className: 'muted',
        text: text || '—',
        style: 'font-size:12px;overflow-wrap:anywhere;max-width:380px;text-align:right',
    });
    node.title = text || '';
    return node;
}

function generalPage() {
    const app = store.appInfo || {};
    const goLaunch = el('button', {
        className: 'btn ghost small',
        text: '去启动页修改',
        type: 'button',
        onclick: () => ctx.navigate('#/launch'),
    });

    return [
        card('常规',
            settingRow('游戏目录', '扫描 versions 文件夹的位置', pathText(store.mcDir)),
            settingRow('玩家名', '离线启动使用的用户名', store.settings.username || 'Player'),
            settingRow('最大内存', '启动参数中的 -Xmx', `${store.settings.maxMem || 4096} MB`),
            settingRow('窗口尺寸', '游戏窗口的初始大小',
                `${store.settings.width || 854} × ${store.settings.height || 480}`),
            settingRow('进入服务器', '留空则进入单人界面', store.settings.server || '不自动进入'),
            settingRow('下载源', '官方优先，速度慢时自动切换镜像',
                store.settings.downloadSource === 'official'
                    ? '仅官方'
                    : (store.settings.downloadSource === 'mirror' ? '仅镜像' : '自动')),
            settingRow('修改启动设置', '玩家名、内存、窗口和服务器地址', goLaunch),
        ),
        card('启动器',
            settingRow('版本', 'CMCL（C++ Minecraft Launcher）', app.version || '—'),
            settingRow('界面', 'Win32 + WebView2（本地资源）', '离线可用'),
        ),
    ];
}

function javaPage() {
    const downloadCard = el('div', { className: 'card settings-section' });
    const downloadHead = el('div', { className: 'card-head' });
    downloadHead.append(el('h2', { text: '下载 OpenJDK' }));
    const refreshJdk = el('button', {
        className: 'btn ghost small',
        text: '刷新',
        type: 'button',
        onclick: () => loadJdkReleases(true),
    });
    downloadHead.append(refreshJdk);
    jdkGrid = el('div', { className: 'jdk-grid' });
    downloadCard.append(
        downloadHead,
        el('p', {
            className: 'muted',
            style: 'margin:0 0 12px;font-size:12.5px;line-height:1.7',
            text: '由 Adoptium Temurin 提供，自动校验 SHA-256。优先使用清华镜像，失败时回退到官方 GitHub。',
        }),
        jdkGrid,
    );

    javaProgressCard = el('div', {
        className: 'card settings-section download-progress-card hidden',
    });
    const progressHead = el('div', { className: 'card-head' });
    progressHead.append(el('h2', { text: 'Java 下载进度' }));
    javaProgressStage = el('div', { className: 'progress-stage', text: '准备' });
    const progressBar = el('div', { className: 'progress' });
    javaProgressFill = el('div', { className: 'progress-fill' });
    progressBar.append(javaProgressFill);
    javaProgressPercent = el('span', { className: 'action-percent', text: '0%' });
    javaProgressMeta = el('div', { className: 'progress-meta muted', text: '' });
    javaProgressFile = el('div', { className: 'progress-file muted', text: '' });
    javaProgressCancel = el('button', {
        className: 'btn ghost small',
        type: 'button',
        text: '取消',
        onclick: () => downloadStore.cancel(),
    });
    javaProgressCard.append(
        progressHead,
        javaProgressStage,
        progressBar,
        el('div', {
            className: 'progress-meta-row',
            style: 'display:flex;align-items:center;justify-content:space-between;gap:12px',
        }, [javaProgressMeta, javaProgressPercent]),
        javaProgressFile,
        javaProgressCancel,
    );

    const tableCard = el('div', { className: 'card table-card' });
    const head = el('div', { className: 'card-head' });
    head.append(el('h2', { text: '已检测到的 Java' }));

    const detect = el('button', {
        className: 'btn ghost small',
        text: '重新检测',
        type: 'button',
        onclick: () => loadJava(tableBody, true),
    });
    head.append(detect);

    const table = el('table', { className: 'instance-table jdk-table' });
    const thead = el('thead');
    const headRow = el('tr');
    const columns = [
        ['版本', 'col-java-version'],
        ['位数', 'col-bits'],
        ['主版本', 'col-major'],
        ['路径', 'col-path'],
        ['操作', 'col-actions'],
    ];
    for (const [title, className] of columns) {
        headRow.append(el('th', { className, text: title }));
    }
    thead.append(headRow);
    const tableBody = el('tbody');
    javaTableBody = tableBody;
    table.append(thead, tableBody);
    tableCard.append(head, el('div', { className: 'table-scroll' }, [table]));

    const loadingRow = el('tr');
    const loadingCell = el('td', { className: 'muted', text: '正在检测 Java…' });
    loadingCell.colSpan = 5;
    loadingRow.append(loadingCell);
    tableBody.append(loadingRow);

    const note = card('选择与校验规则',
        el('p', {
            className: 'muted',
            style: 'margin:0;font-size:12.5px;line-height:1.7',
            text: '启动前会检查版本要求的最低 Java 主版本；低于要求会拒绝启动并提示，不再回退到旧 Java。Forge 1.6.1–1.7.2 使用 Java 7/8，现代版本优先 64 位 Java。',
        }),
    );

    queueMicrotask(() => {
        loadJdkReleases(false);
        loadJava(tableBody, false);
    });
    if (unsubscribeDownload) unsubscribeDownload();
    unsubscribeDownload = downloadStore.subscribe(renderJavaProgress);
    return [downloadCard, javaProgressCard, tableCard, note];
}

function renderJdkCards() {
    if (!jdkGrid) return;
    clear(jdkGrid);
    jdkButtons = [];

    if (!lastReleases.length) {
        jdkGrid.append(el('div', {
            className: 'muted',
            text: '正在获取 Adoptium 版本列表…',
        }));
        return;
    }

    for (const release of lastReleases) {
        const jdkCard = el('div', { className: 'jdk-card' });
        const top = el('div', { className: 'jdk-card-top' });
        top.append(
            el('strong', { className: 'jdk-feature', text: `Java ${release.feature}` }),
            el('span', { className: 'badge latest', text: 'LTS' }),
        );
        const installed = detectedJavas.find((java) =>
            java.managed && java.major === release.feature);
        const button = el('button', {
            className: 'btn primary small jdk-download',
            type: 'button',
            text: installed ? '重新安装' : '下载',
            onclick: () => downloadJdk(release.feature, Boolean(installed)),
        });
        button.disabled = downloadStore.busy;
        jdkButtons.push({ release, button });

        jdkCard.append(
            top,
            el('div', { className: 'jdk-version', text: release.version || '最新版本' }),
            el('div', {
                className: 'jdk-meta muted',
                text: `${fmtBytes(release.size)}${installed ? ' · 已安装' : ''}`,
            }),
            button,
        );
        jdkGrid.append(jdkCard);
    }
}

async function loadJdkReleases(force) {
    const token = ++jdkSeq;
    setStatus('正在获取 OpenJDK 版本列表…');
    try {
        const result = await call('java.available', { force });
        if (token !== jdkSeq) return;
        lastReleases = Array.isArray(result.releases) ? result.releases : [];
        renderJdkCards();
        setStatus(`可下载 ${lastReleases.length} 个 OpenJDK 版本`);
    } catch (error) {
        if (token !== jdkSeq) return;
        lastReleases = [];
        clear(jdkGrid);
        jdkGrid.append(el('div', { className: 'alert', text: formatError(error) }));
        const retry = el('button', {
            className: 'btn ghost small',
            text: '重试',
            type: 'button',
            onclick: () => loadJdkReleases(true),
        });
        jdkGrid.append(retry);
        setStatus('OpenJDK 版本列表获取失败');
    }
}

async function downloadJdk(feature, force) {
    if (downloadStore.busy) return;
    try {
        await downloadStore.start('java', { feature, force: Boolean(force) });
        if (!ctx) return;
        setStatus(`Java ${feature} 安装完成`);
        await loadJava(javaTableBody, true);
    } catch (error) {
        const message = formatError(error).split('\n')[0];
        setStatus(message.includes('取消')
            ? '已取消 Java 下载'
            : 'Java 下载失败：' + message);
    }
}

function renderJavaProgress(state) {
    if (!javaProgressCard) return;
    const mine = state.mode === 'java';
    const active = mine && (state.busy || state.done || Boolean(state.error));
    const wasHidden = javaProgressCard.classList.contains('hidden');
    javaProgressCard.classList.toggle('hidden', !active);
    if (active && wasHidden) {
        javaProgressCard.scrollIntoView({ block: 'nearest', behavior: 'smooth' });
    }
    for (const item of jdkButtons) {
        item.button.disabled = state.busy;
    }
    if (!active) return;

    const percent = state.done ? 100 : Math.round(state.percent());
    const cancelled = Boolean(state.error) &&
        (state.error.includes('取消') || state.error.toLowerCase().includes('cancel'));
    javaProgressFill.style.width = `${Math.max(0, Math.min(100, percent))}%`;
    javaProgressFill.classList.toggle('error', Boolean(state.error) && !cancelled);
    javaProgressFill.classList.toggle('done', state.done);
    javaProgressStage.textContent = state.error
        ? (cancelled ? '已取消' : '下载失败')
        : (state.done ? '安装完成' : (state.stage || '准备'));
    javaProgressPercent.textContent = `${percent}%`;
    const parts = [];
    if (state.total > 0) parts.push(`${state.completed} / ${state.total} 个文件`);
    if (state.totalBytes > 0) {
        parts.push(`${fmtBytes(state.bytes)} / ${fmtBytes(state.totalBytes)}`);
    }
    if (state.speed > 0) parts.push(`${fmtBytes(state.speed)}/s`);
    if (state.eta() > 0) parts.push(`剩余 ${fmtDuration(state.eta())}`);
    javaProgressMeta.textContent = parts.join(' · ');
    javaProgressFile.textContent = state.file || '';
    javaProgressCancel.classList.toggle('hidden', !state.busy);
}

async function deleteManagedJava(java) {
    if (javaActionBusy) return;
    javaActionBusy = true;
    try {
        const confirmed = await confirmDialog({
            title: `删除 Java ${java.version}？`,
            message: `这是一个由 CMCL 下载并管理的 JDK。\n\n` +
                `${java.path}\n\n` +
                '删除会把整个 JDK 目录移到回收站，正在使用它的游戏需要重新选择 Java。',
            confirmText: '移到回收站',
            danger: true,
        });
        if (!confirmed) return;

        setStatus(`正在删除 Java ${java.version}…`);
        await call('java.delete', { path: java.path });
        if (!ctx) return;
        await loadJava(javaTableBody, true);
        setStatus(`已删除 Java ${java.version}，可在回收站还原`);
    } catch (error) {
        setStatus('删除 Java 失败：' + formatError(error).split('\n')[0]);
    } finally {
        javaActionBusy = false;
    }
}

async function uninstallLocalJava(java) {
    if (javaActionBusy) return;
    javaActionBusy = true;
    try {
        const confirmed = await confirmDialog({
            title: `卸载 Java ${java.version}？`,
            message: '这个 Java 是磁盘本地加载的，CMCL 不会直接删除它的文件。\n\n' +
                '将打开它自带的卸载程序；找不到时打开 Windows 的“应用和功能”。\n' +
                '卸载完成后回到这里点“重新检测”。',
            confirmText: '打开卸载程序',
        });
        if (!confirmed) return;

        await call('java.uninstall', { path: java.path });
        setStatus('已打开卸载程序，完成后请点“重新检测”');
    } catch (error) {
        setStatus('打开卸载程序失败：' + formatError(error).split('\n')[0]);
    } finally {
        javaActionBusy = false;
    }
}

async function loadJava(tbody, force) {
    const token = ++javaSeq;
    setStatus('正在检测 Java…');
    try {
        const result = await call('java.list', { mc_dir: store.mcDir, force: Boolean(force) });
        if (token !== javaSeq) return;
        const javas = Array.isArray(result.javas) ? result.javas : [];
        detectedJavas = javas;
        renderJdkCards();
        if (!tbody) {
            setStatus(`检测到 ${javas.length} 个 Java`);
            return;
        }
        clear(tbody);
        if (!javas.length) {
            const row = el('tr');
            const cell = el('td', {
                className: 'muted',
                text: '没有检测到 Java。可以在上方下载 OpenJDK，或安装 64 位 Java 17/21。',
            });
            cell.colSpan = 5;
            row.append(cell);
            tbody.append(row);
            setStatus('没有检测到 Java');
            return;
        }
        for (const java of javas) {
            const row = el('tr');
            const nameCell = el('td', { className: 'name-cell' });
            nameCell.append(el('span', { text: `Java ${java.version}` }));
            nameCell.append(el('span', {
                className: 'badge ' + (java.managed ? 'managed' : 'local'),
                style: 'margin-left:6px',
                text: java.managed ? 'CMCL 下载' : '磁盘本地加载',
            }));

            const actionCell = el('td', { className: 'col-actions' });
            if (java.managed) {
                actionCell.append(el('button', {
                    className: 'btn danger small',
                    type: 'button',
                    text: '删除',
                    title: '把这个 JDK 移到回收站',
                    onclick: () => deleteManagedJava(java),
                }));
            } else {
                actionCell.append(el('button', {
                    className: 'btn ghost small',
                    type: 'button',
                    text: '卸载',
                    title: '打开这个 Java 自带的卸载程序',
                    onclick: () => uninstallLocalJava(java),
                }));
            }

            row.append(
                nameCell,
                el('td', { className: 'col-bits', text: `${java.bits} 位` }),
                el('td', { className: 'col-major', text: String(java.major) }),
                el('td', {
                    className: 'mono col-path path-cell',
                    text: java.path,
                    title: java.path,
                }),
                actionCell,
            );
            tbody.append(row);
        }
        setStatus(`检测到 ${javas.length} 个 Java`);
    } catch (error) {
        if (token !== javaSeq) return;
        if (!tbody) return;
        clear(tbody);
        const row = el('tr');
        const cell = el('td', { className: 'muted', text: formatError(error) });
        cell.colSpan = 5;
        row.append(cell);
        tbody.append(row);
        setStatus('Java 检测失败');
    }
}

function stopDevicePolling() {
    if (devicePollTimer) {
        clearTimeout(devicePollTimer);
        devicePollTimer = null;
    }
    devicePollBusy = false;
}

function scheduleDevicePoll(delayMs) {
    stopDevicePolling();
    devicePollTimer = setTimeout(pollDeviceLogin, Math.max(1000, delayMs));
}

function setDeviceMessage(message, isError) {
    if (deviceLogin) deviceLogin.message = message || '';
    const node = deviceLogin?.statusEl;
    if (node) {
        node.textContent = deviceLogin?.message || '正在等待你在浏览器中完成登录…';
        node.classList.toggle('error', Boolean(isError));
    }
}

async function refreshAccount() {
    const token = ++accountSeq;
    if (!accountHolder) return;
    clear(accountHolder);
    const loading = card('Microsoft 账号',
        el('p', { className: 'muted', text: '正在读取本地账号…' }));
    accountHolder.append(loading);

    try {
        const result = await call('auth.status');
        if (token !== accountSeq || !accountHolder) return;
        accountInfo = result.logged_in ? result.account : null;
        renderAccount();
        setStatus(accountInfo
            ? `已登录 ${accountInfo.name}`
            : '未登录 Microsoft 账号');
    } catch (error) {
        if (token !== accountSeq || !accountHolder) return;
        accountInfo = null;
        accountError = formatError(error);
        renderAccount();
        setStatus('读取账号失败');
    }
}

function renderAccount() {
    if (!accountHolder) return;
    if (deviceLogin) deviceLogin.statusEl = null;
    clear(accountHolder);
    if (accountInfo) {
        accountHolder.append(loggedInCard());
    } else if (deviceLogin) {
        accountHolder.append(deviceLoginCard());
    } else {
        accountHolder.append(signInCard());
    }
    accountHolder.append(accountHelpCard());
}

function signInCard() {
    const node = card('Microsoft 账号');
    node.append(el('p', {
        className: 'muted account-intro',
        text: '登录后，启动游戏会自动使用正版账号：在线服务器可以验证身份，皮肤和披风也会正常显示。' +
            '未登录时按下面的离线玩家名启动。',
    }));

    if (accountError) {
        node.append(el('div', { className: 'alert', text: accountError }));
    }

    const button = el('button', {
        className: 'btn primary',
        type: 'button',
        text: '使用 Microsoft 账号登录',
        onclick: startDeviceLogin,
    });
    node.append(el('div', { className: 'account-actions' }, [button]));
    node.append(el('p', {
        className: 'muted account-note',
        text: '需要已购买 Minecraft: Java Edition。登录在 Microsoft 官方页面完成，CMCL 不会接触你的密码。',
    }));
    return node;
}

function loggedInCard() {
    const node = card('Microsoft 账号');
    const profile = el('div', { className: 'account-profile' });
    const avatar = el('div', {
        className: 'account-avatar',
        text: (accountInfo.name || '?').slice(0, 1).toUpperCase(),
    });
    const info = el('div', { className: 'account-info' });
    info.append(
        el('div', { className: 'account-name', text: accountInfo.name || '—' }),
        el('div', { className: 'account-uuid mono', text: accountInfo.uuid || '' }),
        el('span', {
            className: 'badge ' + (accountInfo.expired ? 'error' : 'managed'),
            text: accountInfo.expired ? '凭证已过期，启动时自动续期' : '正版已登录',
        }),
    );
    profile.append(avatar, info);

    const logout = el('button', {
        className: 'btn ghost small',
        type: 'button',
        text: '退出登录',
        onclick: logoutAccount,
    });
    node.append(profile, el('div', { className: 'account-actions' }, [logout]));
    node.append(el('p', {
        className: 'muted account-note',
        text: '启动游戏时会自动检查并续期登录凭证。退出登录后恢复为离线模式。',
    }));
    return node;
}

function deviceLoginCard() {
    const node = card('登录 Microsoft 账号');
    node.append(el('p', {
        className: 'muted account-intro',
        text: '按下面的步骤在 Microsoft 官方页面完成授权，完成后回到这里即可，无需关闭窗口。',
    }));

    const steps = el('ol', { className: 'device-steps' });
    steps.append(
        el('li', { text: '点击“打开登录页面”（或手动访问 microsoft.com/link）。' }),
        el('li', { text: '输入下面的代码，并选择你的 Microsoft 账号。' }),
        el('li', { text: '同意授权，等待本页面提示登录成功。' }),
    );
    node.append(steps);

    const codeBox = el('div', { className: 'device-code-box' });
    codeBox.append(el('div', { className: 'device-code', text: deviceLogin.user_code }));
    codeBox.append(el('button', {
        className: 'btn ghost small',
        type: 'button',
        text: '复制代码',
        onclick: async () => {
            try {
                await navigator.clipboard.writeText(deviceLogin.user_code);
                setStatus('设备码已复制');
            } catch (_) {
                setStatus('复制失败，请手动输入设备码');
            }
        },
    }));
    node.append(codeBox);

    const open = el('button', {
        className: 'btn primary',
        type: 'button',
        text: '打开登录页面',
        onclick: async () => {
            try {
                await call('app.open_url', { url: deviceLogin.verification_uri });
            } catch (error) {
                setStatus('打开浏览器失败：' + formatError(error).split('\n')[0]);
            }
        },
    });
    const cancel = el('button', {
        className: 'btn ghost',
        type: 'button',
        text: '取消登录',
        onclick: cancelDeviceLogin,
    });
    const statusNode = el('div', {
        className: 'device-status muted',
        text: deviceLogin.message || '正在等待你在浏览器中完成登录…',
    });
    deviceLogin.statusEl = statusNode;

    node.append(el('div', { className: 'account-actions' }, [open, cancel]), statusNode);
    node.append(el('p', {
        className: 'muted account-note',
        text: '设备码 ' + Math.max(1, Math.round((deviceLogin.expires_in || 900) / 60)) +
            ' 分钟内有效。CMCL 只会在本机保存登录凭证。',
    }));
    return node;
}

function accountHelpCard() {
    return card('关于正版验证',
        el('p', {
            className: 'muted account-note',
            style: 'margin:0',
            text: 'CMCL 使用 Microsoft 设备码登录，并通过 Xbox Live / Minecraft Services 校验游戏所有权。' +
                '登录凭证用 Windows DPAPI 加密后保存在本机，只有当前 Windows 用户能解密；' +
                '启动器不会上传账号密码，也不会把令牌发送到第三方服务器。',
        }));
}

async function startDeviceLogin() {
    if (deviceLogin) return;
    accountError = '';
    setStatus('正在向 Microsoft 申请设备码…');
    try {
        const result = await call('auth.device_start');
        if (!ctx) return;
        deviceLogin = {
            ...result,
            message: '正在等待你在浏览器中完成登录…',
            statusEl: null,
        };
        renderAccount();
        setStatus(`请在登录页面输入代码 ${result.user_code}`);
        scheduleDevicePoll(1500);
    } catch (error) {
        accountError = formatError(error);
        renderAccount();
        setStatus('获取设备码失败：' + accountError.split('\n')[0]);
    }
}

async function pollDeviceLogin() {
    if (!deviceLogin || devicePollBusy || !ctx) return;
    devicePollBusy = true;
    let result = null;
    let failure = null;
    try {
        result = await call('auth.device_poll');
    } catch (error) {
        failure = error;
    }
    devicePollBusy = false;

    if (!ctx || !deviceLogin) return;

    if (failure) {
        setDeviceMessage('查询登录状态失败，稍后重试：' +
            formatError(failure).split('\n')[0], true);
        scheduleDevicePoll(5000);
        return;
    }

    if (result.status === 'success') {
        stopDevicePolling();
        deviceLogin = null;
        accountInfo = result.account;
        renderAccount();
        setStatus(result.warning || `已登录 ${result.account?.name || 'Microsoft 账号'}`);
        return;
    }
    if (result.status === 'pending') {
        setDeviceMessage(result.message || '等待你在浏览器中完成登录…', false);
        scheduleDevicePoll((Number(result.interval) || 5) * 1000);
        return;
    }
    if (result.status === 'slow_down') {
        setDeviceMessage('Microsoft 要求放慢查询速度，已自动调整…', false);
        scheduleDevicePoll((Number(result.interval) || 10) * 1000);
        return;
    }

    stopDevicePolling();
    deviceLogin = null;
    accountError = result.message || '登录失败，请重试。';
    renderAccount();
    setStatus(accountError.split('\n')[0]);
}

function cancelDeviceLogin() {
    stopDevicePolling();
    deviceLogin = null;
    accountError = '';
    renderAccount();
    setStatus('已取消 Microsoft 登录');
}

async function logoutAccount() {
    const confirmed = await confirmDialog({
        title: '退出 Microsoft 账号？',
        message: '退出后启动游戏会恢复为离线模式，在线服务器将无法验证正版身份。\n' +
            '下次可以随时重新登录。',
        confirmText: '退出登录',
        danger: true,
    });
    if (!confirmed) return;

    setStatus('正在退出 Microsoft 账号…');
    try {
        await call('auth.logout');
        if (!ctx) return;
        accountInfo = null;
        accountError = '';
        renderAccount();
        setStatus('已退出 Microsoft 账号');
    } catch (error) {
        setStatus('退出登录失败：' + formatError(error).split('\n')[0]);
    }
}

function accountPage() {
    accountHolder = el('div', { className: 'account-page' });
    accountSeq++;
    accountError = '';
    if (!deviceLogin) stopDevicePolling();
    queueMicrotask(refreshAccount);
    return [accountHolder];
}

function logsPage() {
    const app = store.appInfo || {};
    const gameLog = store.mcDir ? `${store.mcDir}\\logs\\latest.log` : '—';
    return [
        card('日志位置',
            settingRow('启动器日志', '每次启动时轮转压缩', pathText(app.log_path || '—')),
            settingRow('游戏日志', '由游戏写入，最新一份为 latest.log', pathText(gameLog)),
            settingRow('崩溃报告', '游戏崩溃时生成', pathText(
                store.mcDir ? `${store.mcDir}\\crash-reports` : '—')),
        ),
        card('保留策略',
            settingRow('轮转', '上一次的启动器日志压缩为 .gz，只保留最近一份', '自动'),
            settingRow('清理', '超过 7 天的 .log.gz 和崩溃报告自动删除', '自动'),
            settingRow('运行总结', '退出后根据日志给出原因和建议，无需读代码', '已启用'),
        ),
    ];
}

function aboutPage() {
    const app = store.appInfo || {};
    return [
        card('CMCL',
            settingRow('全称', 'C++ Minecraft Launcher', '—'),
            settingRow('版本', 'C++ Edition', app.version || '—'),
            settingRow('技术栈', 'C++20 / Win32 / WebView2 / CMake', '无 .NET 运行时'),
            settingRow('当前里程碑', 'M3：下载、校验与修复', '已完成'),
        ),
        card('致谢',
            el('p', {
                className: 'muted',
                style: 'margin:0;font-size:12.5px;line-height:1.7',
                text: '界面结构和交互经验参考了 PCL2（Plain Craft Launcher 2）的开源旧版项目。CMCL 使用全新的 C++ 实现，不复用其代码。',
            }),
        ),
    ];
}

export function mount(context, params) {
    ctx = context;
    const key = SECTIONS[params?.section] ? params.section : 'general';

    if (params?.section && !SECTIONS[params.section]) {
        ctx.navigate('#/settings/general');
        return;
    }

    setBreadcrumb(['设置', SECTIONS[key]]);
    setTopActions([]);
    setSidebar([sectionTabs(key)]);
    setStatus('设置');

    if (key === 'general') ctx.page.append(...generalPage());
    else if (key === 'account') ctx.page.append(...accountPage());
    else if (key === 'java') ctx.page.append(...javaPage());
    else if (key === 'logs') ctx.page.append(...logsPage());
    else ctx.page.append(...aboutPage());
}

export function unmount() {
    javaSeq++;
    jdkSeq++;
    accountSeq++;
    stopDevicePolling();
    deviceLogin = null;
    accountHolder = null;
    accountInfo = null;
    accountError = '';
    // 离开 Java 页时清掉已结束的 Java 任务，避免下次进来看到旧的“完成/失败”卡片。
    if (downloadStore.mode === 'java' && !downloadStore.busy) {
        downloadStore.reset();
    }
    if (unsubscribeDownload) {
        unsubscribeDownload();
        unsubscribeDownload = null;
    }
    detectedJavas = [];
    lastReleases = [];
    jdkButtons = [];
    javaActionBusy = false;
    jdkGrid = null;
    javaTableBody = null;
    javaProgressCard = null;
    ctx = null;
}
