import { el, clear } from './utils.js';

function nodeList(value) {
    if (value === null || value === undefined) return [];
    return Array.isArray(value) ? value : [value];
}

export function setStatus(text) {
    const node = document.getElementById('status');
    if (node) node.textContent = text;
}

export function setMcPath(text) {
    const node = document.getElementById('mc-path');
    if (node) node.textContent = text;
}

export function setBreadcrumb(parts) {
    const root = document.getElementById('breadcrumb');
    if (!root) return;
    clear(root);

    nodeList(parts).forEach((part, index) => {
        if (index > 0) {
            root.append(el('span', { className: 'crumb-sep', text: '›' }));
        }
        if (part instanceof Node) {
            root.append(part);
        } else if (typeof part === 'string') {
            root.append(el('span', { className: 'crumb-current', text: part }));
        } else if (part && part.href) {
            root.append(el('button', {
                className: 'crumb-link',
                text: part.text,
                onclick: () => { location.hash = part.href; },
            }));
        } else {
            root.append(el('span', { className: 'crumb-current', text: part?.text ?? '' }));
        }
    });
}

export function setTopActions(nodes) {
    const root = document.getElementById('top-actions');
    if (!root) return;
    clear(root);
    root.append(...nodeList(nodes).filter(Boolean));
}

export function setSidebar(nodes) {
    const root = document.getElementById('sidebar-slot');
    if (!root) return;
    clear(root);
    root.append(...nodeList(nodes).filter(Boolean));
}

export function setSidebarFooter(nodes) {
    const root = document.getElementById('sidebar-footer');
    if (!root) return;
    clear(root);
    root.append(...nodeList(nodes).filter(Boolean));
}

export function clearShellSlots() {
    setSidebar([]);
    setSidebarFooter([]);
    setTopActions([]);
}

export function confirmDialog({
    title = '请确认',
    message = '',
    confirmText = '确定',
    cancelText = '取消',
    danger = false,
} = {}) {
    return new Promise((resolve) => {
        const overlay = el('div', { className: 'modal-overlay' });
        const modal = el('div', {
            className: 'modal',
            role: 'dialog',
            'aria-modal': 'true',
        });
        modal.append(el('h3', { text: title }));
        if (message) modal.append(el('p', { text: message }));

        const actions = el('div', { className: 'modal-actions' });
        const cancel = el('button', { className: 'btn ghost', text: cancelText, type: 'button' });
        const confirm = el('button', {
            className: 'btn ' + (danger ? 'danger' : 'primary'),
            text: confirmText,
            type: 'button',
        });
        actions.append(cancel, confirm);
        modal.append(actions);
        overlay.append(modal);
        document.body.append(overlay);

        const done = (value) => {
            overlay.remove();
            document.removeEventListener('keydown', onKey);
            resolve(value);
        };
        const onKey = (event) => {
            if (event.key === 'Escape') done(false);
            if (event.key === 'Enter') done(true);
        };

        cancel.addEventListener('click', () => done(false));
        confirm.addEventListener('click', () => done(true));
        overlay.addEventListener('click', (event) => {
            if (event.target === overlay) done(false);
        });
        document.addEventListener('keydown', onKey);
        confirm.focus();
    });
}

export function promptDialog({
    title = '输入',
    message = '',
    label = '',
    value = '',
    placeholder = '',
    confirmText = '确定',
    cancelText = '取消',
    validate = null,
} = {}) {
    return new Promise((resolve) => {
        const overlay = el('div', { className: 'modal-overlay' });
        const modal = el('div', {
            className: 'modal',
            role: 'dialog',
            'aria-modal': 'true',
        });
        modal.append(el('h3', { text: title }));
        if (message) modal.append(el('p', { text: message }));

        const field = el('label', { style: 'display:block;margin-top:12px' });
        if (label) field.append(el('span', { className: 'muted', text: label }));
        const input = el('input', {
            type: 'text',
            value,
            placeholder,
            spellcheck: 'false',
            autocomplete: 'off',
        });
        field.append(input);
        const errorNode = el('div', { className: 'modal-error' });

        const actions = el('div', { className: 'modal-actions' });
        const cancel = el('button', { className: 'btn ghost', text: cancelText, type: 'button' });
        const confirm = el('button', { className: 'btn primary', text: confirmText, type: 'button' });
        actions.append(cancel, confirm);

        modal.append(field, errorNode, actions);
        overlay.append(modal);
        document.body.append(overlay);

        const done = (result) => {
            overlay.remove();
            document.removeEventListener('keydown', onKey);
            resolve(result);
        };
        const submit = () => {
            const text = input.value.trim();
            if (!text) {
                errorNode.textContent = '名称不能为空';
                return;
            }
            if (validate) {
                const error = validate(text);
                if (error) {
                    errorNode.textContent = error;
                    return;
                }
            }
            done(text);
        };
        const onKey = (event) => {
            if (event.key === 'Escape') done(null);
            if (event.key === 'Enter') submit();
        };

        cancel.addEventListener('click', () => done(null));
        confirm.addEventListener('click', submit);
        overlay.addEventListener('click', (event) => {
            if (event.target === overlay) done(null);
        });
        document.addEventListener('keydown', onKey);

        input.focus();
        input.select();
    });
}

export function validateInstanceName(name) {
    if (!name) return '名称不能为空';
    if (name === '.' || name === '..') return '名称无效';
    if (/[<>:"/\\|?*\u0000-\u001f]/.test(name)) {
        return '不能包含 < > : " / \\ | ? * 等字符';
    }
    if (/[. ]$/.test(name)) return '不能以空格或点结尾';
    if (/^(con|prn|aux|nul|com[1-9]|lpt[1-9])$/i.test(name)) {
        return '这是 Windows 保留名称';
    }
    return '';
}
