export function el(tag, props = {}, children = []) {
    const node = document.createElement(tag);

    for (const [key, value] of Object.entries(props)) {
        if (value === undefined || value === null) continue;
        if (key === 'className') {
            node.className = value;
        } else if (key === 'text') {
            node.textContent = value;
        } else if (key === 'html') {
            node.innerHTML = value;
        } else if (key.startsWith('on') && typeof value === 'function') {
            node.addEventListener(key.slice(2).toLowerCase(), value);
        } else if (key === 'checked') {
            node.checked = Boolean(value);
        } else if (key === 'value') {
            node.value = value;
        } else {
            node.setAttribute(key, value);
        }
    }

    const list = Array.isArray(children) ? children : [children];
    for (const child of list) {
        if (child === null || child === undefined) continue;
        node.append(child instanceof Node ? child : document.createTextNode(String(child)));
    }
    return node;
}

export function clear(node) {
    if (!node) return;
    while (node.firstChild) node.removeChild(node.firstChild);
}

export function badgeClass(type) {
    if (type === 'release') return 'release';
    if (type === 'snapshot') return 'snapshot';
    if (type === 'old_beta' || type === 'old_alpha') return 'old';
    return '';
}

export const LOADER_ORDER = ['vanilla', 'forge', 'neoforge', 'fabric', 'quilt', 'optifine'];

export function loaderLabel(loader) {
    switch (loader) {
        case 'vanilla': return '原版';
        case 'forge': return 'Forge';
        case 'neoforge': return 'NeoForge';
        case 'fabric': return 'Fabric';
        case 'quilt': return 'Quilt';
        case 'optifine': return 'OptiFine';
        default: return '其他';
    }
}

export function loaderBadgeClass(loader) {
    return 'badge loader ' + (LOADER_ORDER.includes(loader) ? loader : 'other');
}

export function typeLabel(type) {
    switch (type) {
        case 'release': return '正式版';
        case 'snapshot': return '快照';
        case 'old_beta': return '旧测试版';
        case 'old_alpha': return '旧预览版';
        default: return type || '未知';
    }
}

export function fmtBytes(bytes) {
    const value = Number(bytes) || 0;
    if (value >= 1024 * 1024 * 1024) {
        return `${(value / (1024 * 1024 * 1024)).toFixed(2)} GB`;
    }
    if (value >= 1024 * 1024) {
        return `${(value / (1024 * 1024)).toFixed(1)} MB`;
    }
    if (value >= 1024) {
        return `${(value / 1024).toFixed(0)} KB`;
    }
    return `${value} B`;
}

export function fmtDuration(seconds) {
    const total = Math.max(0, Math.round(Number(seconds) || 0));
    const minutes = Math.floor(total / 60);
    const rest = total % 60;
    return minutes > 0 ? `${minutes} 分 ${rest} 秒` : `${rest} 秒`;
}

export function formatError(error) {
    if (!error) return '未知错误';
    if (typeof error === 'string') return error;
    let text = error.message || String(error);
    if (error.context) text += '\n' + error.context;
    return text;
}

export function classifyLine(line) {
    if (!line) return '';
    if (/\[(ERROR|SEVERE|FATAL|错误|致命)\]/i.test(line) || /exception/i.test(line)) {
        return 'err';
    }
    if (/\[(WARN|WARNING|警告)\]/i.test(line)) return 'warn';
    return '';
}
