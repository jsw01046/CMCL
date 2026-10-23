import { clear, formatError } from './utils.js';
import { setStatus, clearShellSlots } from './ui.js';
import * as LaunchPage from './pages/launch.js';
import * as InstancesPage from './pages/instances.js';
import * as InstanceDetailPage from './pages/instance-detail.js';
import * as InstanceResourcesPage from './pages/resources.js';
import * as DownloadPage from './pages/download.js';
import * as SettingsPage from './pages/settings.js';

const ROUTES = [
    {
        pattern: /^\/launch$/,
        page: LaunchPage,
        nav: 'launch',
        title: '启动',
    },
    {
        pattern: /^\/instances$/,
        page: InstancesPage,
        nav: 'instances',
        title: '版本管理',
    },
    {
        pattern: /^\/instances\/([^/]+)\/resources$/,
        page: InstanceResourcesPage,
        nav: 'instances',
        title: '资源管理',
        params: (match) => ({ id: decodeURIComponent(match[1]) }),
    },
    {
        pattern: /^\/instances\/([^/]+)$/,
        page: InstanceDetailPage,
        nav: 'instances',
        title: '版本详情',
        params: (match) => ({ id: decodeURIComponent(match[1]) }),
    },
    {
        pattern: /^\/download(?:\/([^/]+))?$/,
        page: DownloadPage,
        nav: 'download',
        title: '下载',
        params: (match) => ({ section: match[1] || 'install' }),
    },
    {
        pattern: /^\/settings(?:\/([^/]+))?$/,
        page: SettingsPage,
        nav: 'settings',
        title: '设置',
        params: (match) => ({ section: match[1] || 'general' }),
    },
];

let currentPage = null;
let mounted = false;

function navActive(name) {
    document.querySelectorAll('.nav-item').forEach((item) => {
        item.classList.toggle('active', item.dataset.nav === name);
    });
}

function showFatal(error) {
    const page = document.getElementById('page');
    if (!page) return;
    clear(page);
    const card = document.createElement('div');
    card.className = 'card';
    const title = document.createElement('h2');
    title.textContent = '页面加载失败';
    title.style.margin = '0 0 8px';
    const text = document.createElement('div');
    text.className = 'alert';
    text.textContent = formatError(error);
    card.append(title, text);
    page.append(card);
    setStatus('页面加载失败');
}

function context() {
    return {
        page: document.getElementById('page'),
        navigate,
        setStatus,
    };
}

export function navigate(hash) {
    if (!hash) return;
    if (location.hash === hash) {
        render();
        return;
    }
    location.hash = hash;
}

export function currentPath() {
    return location.hash.replace(/^#/, '') || '/launch';
}

function render() {
    const path = currentPath();

    let route = null;
    let match = null;
    for (const candidate of ROUTES) {
        match = path.match(candidate.pattern);
        if (match) {
            route = candidate;
            break;
        }
    }

    if (!route) {
        navigate('#/launch');
        return;
    }

    let params = {};
    if (route.params) {
        try {
            params = route.params(match);
        } catch (_) {
            navigate('#/launch');
            return;
        }
    }

    try {
        currentPage?.unmount?.();
    } catch (error) {
        console.error('[router] unmount failed', error);
    }
    currentPage = null;

    navActive(route.nav);
    document.title = `CMCL - ${route.title} | C++ Minecraft Launcher`;
    clearShellSlots();

    const page = document.getElementById('page');
    if (page) clear(page);

    const ctx = context();
    try {
        const result = route.page.mount(ctx, params);
        if (result && typeof result.catch === 'function') {
            result.catch(showFatal);
        }
        currentPage = route.page;
    } catch (error) {
        showFatal(error);
    }
}

export function startRouter() {
    if (mounted) return;
    mounted = true;

    const nav = document.getElementById('nav');
    nav?.addEventListener('click', (event) => {
        const button = event.target.closest('[data-route]');
        if (!button) return;
        event.preventDefault();
        navigate(button.dataset.route);
    });

    window.addEventListener('hashchange', render);

    if (!location.hash || location.hash === '#') {
        navigate('#/launch');
    } else {
        render();
    }
}
