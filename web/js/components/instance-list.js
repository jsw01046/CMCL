import {
    el,
    clear,
    loaderLabel,
    loaderBadgeClass,
    LOADER_ORDER,
} from '../utils.js';

export function createInstanceList({ onSelect, onActivate = null } = {}) {
    const root = el('div', { className: 'instance-list' });
    let items = [];
    let selected = null;
    let filter = '';
    let loaderFilter = '';

    function render() {
        clear(root);
        const keyword = filter.trim().toLowerCase();
        const visible = items.filter((item) => {
            if (loaderFilter === 'other') {
                if (LOADER_ORDER.includes(item.loader)) return false;
            } else if (loaderFilter && item.loader !== loaderFilter) {
                return false;
            }
            return !keyword || item.id.toLowerCase().includes(keyword);
        });

        if (!visible.length) {
            root.append(el('div', {
                className: 'empty',
                text: items.length ? '没有匹配的版本' : '没有找到版本',
            }));
            return;
        }

        for (const item of visible) {
            const card = el('div', {
                className: 'instance' +
                    (item.id === selected ? ' active' : '') +
                    (item.valid ? '' : ' invalid'),
            });
            card.dataset.id = item.id;
            card.title = item.id;

            const top = el('div', { className: 'instance-top' });
            top.append(el('span', { className: 'instance-name', text: item.id }));

            const badge = el('span', {
                className: item.valid
                    ? loaderBadgeClass(item.loader)
                    : 'badge error',
                text: item.valid ? loaderLabel(item.loader) : '无效',
            });
            top.append(badge);

            if (item.isolate) {
                top.append(el('span', { className: 'badge isolate', text: '隔离' }));
            }

            const sub = el('div', { className: 'instance-sub' });
            if (!item.valid) {
                sub.textContent = item.error || '版本文件缺失';
            } else if (item.inherits_from) {
                sub.textContent = '继承 ' + item.inherits_from;
            } else {
                sub.textContent = item.main_class || '—';
            }

            card.append(top, sub);
            card.addEventListener('click', () => onSelect?.(item.id));
            card.addEventListener('dblclick', () => onActivate?.(item.id));
            root.append(card);
        }
    }

    return {
        element: root,
        setData(next, selectedId = null) {
            items = Array.isArray(next) ? next : [];
            selected = selectedId;
            render();
        },
        setSelected(id) {
            selected = id;
            render();
        },
        setFilter(text) {
            filter = text || '';
            render();
        },
        setLoader(loader) {
            loaderFilter = loader || '';
            render();
        },
        get selected() {
            return selected;
        },
    };
}

export function createInstanceSearch(list) {
    const input = el('input', {
        className: 'search',
        type: 'search',
        placeholder: '搜索版本…',
        spellcheck: 'false',
    });
    input.addEventListener('input', () => list.setFilter(input.value));

    const select = el('select', { className: 'loader-select', title: '按加载器筛选' });
    select.append(el('option', { value: '', text: '全部' }));
    for (const loader of LOADER_ORDER) {
        select.append(el('option', { value: loader, text: loaderLabel(loader) }));
    }
    select.append(el('option', { value: 'other', text: '其他' }));
    select.addEventListener('change', () => list.setLoader(select.value));

    const wrap = el('div', { className: 'search-wrap' }, [input, select]);
    return wrap;
}
