import { el, clear, fmtDuration } from '../utils.js';
import { setStatus } from '../ui.js';
import { openCrashReport } from './crash-report.js';

const LOG_LIMIT = 4000;

const CATEGORY_LABELS = {
    memory: '内存',
    java: 'Java',
    loader: '加载器',
    dependency: '依赖',
    conflict: '模组冲突',
    mixin: 'Mixin',
    mod_crash: '模组崩溃',
    version: '版本',
    native: '原生库',
    jvm: 'JVM',
    graphics: '显卡',
    crash: '崩溃',
    launcher: '启动器',
    unknown: '未知',
};

function summaryText(data, suggestions, meta) {
    const findings = Array.isArray(data.findings) ? data.findings : [];
    const findingLines = findings.length
        ? [
            `发现的问题（${findings.length}）：`,
            ...findings.map((finding, index) => {
                let line = `${index + 1}. ${finding.title || ''}`;
                if (finding.time) line += `（${finding.time}）`;
                if (finding.mod_a) {
                    line += ` · ${finding.mod_a}`;
                    if (finding.mod_b) line += ` ↔ ${finding.mod_b}`;
                }
                return line;
            }),
        ]
        : [];
    return [
        `运行结果：${data.title || ''}`,
        `原因：${data.detail || ''}`,
        ...findingLines,
        suggestions.length ? '建议：' : '',
        ...suggestions.map((suggestion, index) => `${index + 1}. ${suggestion}`),
        meta.join(' · '),
    ].filter(Boolean).join('\n');
}

export function createLogView() {
    const card = el('div', { className: 'card log-card' });

    const head = el('div', { className: 'card-head' });
    head.append(el('h2', { text: '运行日志' }));

    const actions = el('div', { className: 'log-actions' });
    const stateNode = el('span', { className: 'muted', text: '未启动' });
    const clearButton = el('button', {
        className: 'btn ghost small',
        text: '清空',
        type: 'button',
    });
    actions.append(stateNode, clearButton);
    head.append(actions);

    const summary = el('div', { className: 'run-summary hidden' });
    const summaryTop = el('div', { className: 'summary-top' });
    const summaryTitle = el('strong', { text: '' });
    const copyButton = el('button', {
        className: 'btn ghost small',
        text: '复制总结',
        type: 'button',
    });
    const exportButton = el('button', {
        className: 'btn ghost small',
        text: '导出报告',
        type: 'button',
    });
    summaryTop.append(summaryTitle, exportButton, copyButton);

    const summaryDetail = el('p', { text: '' });
    const findingsBox = el('div', { className: 'findings hidden' });
    const summaryList = el('ul');
    const unknownBox = el('div', { className: 'unknown-box hidden' });
    const unknownActions = el('div', { className: 'unknown-actions' });
    unknownActions.append(el('button', {
        className: 'btn primary small',
        type: 'button',
        text: '导出崩溃报告…',
        onclick: () => {
            if (currentSummary) openCrashReport(currentSummary);
        },
    }));
    unknownBox.append(
        el('strong', { text: '未识别的崩溃特征' }),
        el('p', {
            text: '日志里没有匹配到已知特征。可以导出完整报告，复制后发送给开发者。',
        }),
        unknownActions,
    );
    const summaryMeta = el('div', { className: 'meta', text: '' });
    summary.append(summaryTop, summaryDetail, findingsBox, summaryList,
        unknownBox, summaryMeta);

    const pre = el('pre', { className: 'log' });

    card.append(head, summary, pre);

    let lastSummaryText = '';
    let lastSummarySignature = '';
    let currentSummary = null;

    function findingNode(finding) {
        const severity = finding.severity === 'warn' ? 'warn' : 'error';
        const node = el('div', { className: 'finding ' + severity });
        const head = el('div', { className: 'finding-head' });
        const title = el('div', { className: 'finding-title' });
        title.append(el('strong', { text: finding.title || '问题' }));
        const category = CATEGORY_LABELS[finding.category] || finding.category;
        if (category) {
            title.append(el('span', { className: 'badge', text: category }));
        }
        if (finding.id) {
            title.append(el('span', {
                className: 'finding-id muted',
                text: finding.id,
            }));
        }
        if (finding.time) {
            title.append(el('span', {
                className: 'finding-time muted',
                text: finding.time,
            }));
        }
        head.append(title);
        node.append(head);

        if (finding.mod_a || finding.mod_b) {
            const mods = el('div', { className: 'finding-mods' });
            mods.append(el('span', { className: 'muted', text: '涉及模组：' }));
            mods.append(el('strong', { text: finding.mod_a || '未知' }));
            if (finding.mod_b) {
                mods.append(el('span', { text: ' ↔ ' }));
                mods.append(el('strong', { text: finding.mod_b }));
            }
            node.append(mods);
        }

        if (finding.detail) {
            node.append(el('p', { className: 'finding-detail', text: finding.detail }));
        }

        if (finding.evidence) {
            const details = el('details', { className: 'finding-evidence' });
            details.append(
                el('summary', { text: '查看日志证据' }),
                el('pre', { text: finding.evidence }),
            );
            node.append(details);
        }

        const suggestions = Array.isArray(finding.suggestions)
            ? finding.suggestions
            : [];
        if (suggestions.length) {
            const list = el('ul', { className: 'finding-suggestions' });
            for (const suggestion of suggestions) {
                list.append(el('li', { text: suggestion }));
            }
            node.append(list);
        }
        return node;
    }

    function append(text, kind = '') {
        const atBottom = pre.scrollTop + pre.clientHeight >= pre.scrollHeight - 48;
        const line = el('span', {
            className: 'line' + (kind ? ' ' + kind : ''),
            text: String(text ?? '').replace(/\r$/, ''),
        });
        pre.append(line);
        while (pre.children.length > LOG_LIMIT) pre.removeChild(pre.firstChild);
        if (atBottom) pre.scrollTop = pre.scrollHeight;
    }

    function reset() {
        clear(pre);
        hideSummary();
        setState('未启动');
    }

    function hideSummary() {
        summary.className = 'run-summary hidden';
        lastSummaryText = '';
        lastSummarySignature = '';
        currentSummary = null;
        clear(findingsBox);
        findingsBox.classList.add('hidden');
        unknownBox.classList.add('hidden');
    }

    function showSummary(data) {
        if (!data) return;
        const signature = JSON.stringify(data);
        if (signature === lastSummarySignature) return;
        lastSummarySignature = signature;

        const severity = data.severity === 'error'
            ? 'error'
            : (data.severity === 'warn' ? 'warn' : 'info');
        summary.className = 'run-summary ' + severity;
        currentSummary = data;

        summaryTitle.textContent = data.title || '运行总结';
        summaryDetail.textContent = data.detail || '';

        clear(findingsBox);
        const findings = Array.isArray(data.findings) ? data.findings : [];
        if (findings.length) {
            findingsBox.classList.remove('hidden');
            for (const finding of findings.slice(0, 8)) {
                findingsBox.append(findingNode(finding));
            }
            if (findings.length > 8) {
                findingsBox.append(el('div', {
                    className: 'finding-more muted',
                    text: `还有 ${findings.length - 8} 条分析结果，详见导出的报告。`,
                }));
            }
        } else {
            findingsBox.classList.add('hidden');
        }
        unknownBox.classList.toggle('hidden', !data.unknown);

        clear(summaryList);
        const suggestions = Array.isArray(data.suggestions) ? data.suggestions : [];
        for (const suggestion of suggestions) {
            summaryList.append(el('li', { text: suggestion }));
        }

        const meta = [];
        if (!data.manual && data.exit_code !== undefined && data.exit_code !== null) {
            meta.push(`退出码 ${data.exit_code}`);
        }
        if (data.duration_seconds !== undefined && data.duration_seconds !== null) {
            meta.push(`运行 ${fmtDuration(data.duration_seconds)}`);
        }
        summaryMeta.textContent = meta.join(' · ');

        lastSummaryText = summaryText(data, suggestions, meta);

        append('', '');
        append('========== 运行总结 ==========', 'sys');
        append(data.title || '运行总结', severity === 'error' ? 'err' : 'sys');
        if (data.detail) append(data.detail);
        for (const finding of findings) {
            append(`  [${finding.category}] ${finding.title}` +
                (finding.time ? `（${finding.time}）` : ''), 'err');
            if (finding.event) append('    ' + finding.event);
        }
        for (const suggestion of suggestions) append('  · ' + suggestion);
        if (meta.length) append('  ' + meta.join(' · '));
        append('================================', 'sys');

        setStatus(data.title || '运行结束');
        setState(data.manual
            ? '已停止'
            : (severity === 'error' ? '异常退出' : '已退出'));
    }

    async function copySummary() {
        if (!lastSummaryText) return;
        try {
            await navigator.clipboard.writeText(lastSummaryText);
            setStatus('运行总结已复制');
            return;
        } catch (_) {
            // 回退到旧版复制方式。
        }
        const area = el('textarea', { value: lastSummaryText });
        area.style.position = 'fixed';
        area.style.opacity = '0';
        document.body.append(area);
        area.select();
        try {
            document.execCommand('copy');
            setStatus('运行总结已复制');
        } catch (_) {
            setStatus('复制失败，请手动选择文本');
        }
        area.remove();
    }

    function setState(text) {
        stateNode.textContent = text;
    }

    clearButton.addEventListener('click', () => {
        reset();
        setStatus('日志已清空');
    });
    copyButton.addEventListener('click', () => copySummary());
    exportButton.addEventListener('click', () => {
        if (currentSummary) openCrashReport(currentSummary);
    });

    return {
        element: card,
        append,
        reset,
        hideSummary,
        showSummary,
        setState,
        get summaryShown() {
            return Boolean(lastSummaryText);
        },
    };
}
