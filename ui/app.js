"use strict";

const byId = id => document.getElementById(id);
const form = byId("profile-form");
const dialog = byId("profile-dialog");
const node = byId("node");
const chromePath = byId("chrome-path");
const status = byId("status");
const formStatus = byId("form-status");

const errors = {
    invalid_name: "请输入有效的 Target 别名。",
    invalid_data_dir: "请输入有效的 Windows 本地目录绝对路径。",
    invalid_chrome_path: "请选择已有的 chrome.exe 文件。",
    duplicate_data_dir: "已有实例使用这个数据目录，请为每个实例指定独立的目录。",
    select_node: "请选择上游节点或直连。",
    permission_denied: "权限不足。请在插件中心检查已授予的插件权限和上游节点。",
    upstream_not_found: "上游节点已不存在，请选择其他节点。",
    upstream_invalid: "上游节点配置无效，请检查 Ghost 的节点设置。",
    target_not_found: "Target 已不存在，列表会自动更新。",
    target_running: "实例已在运行。关闭 Chrome 后再编辑或启动。",
    launch_failed: "Ghost 启动 Chrome 失败，请查看 Ghost 日志。",
    user_denied: "已取消启动。",
    approval_timeout: "启动审批超时，请重新点击启动。",
    rate_limited: "请求过于频繁，请稍后再试。",
    target_changed: "审批期间配置发生变化，请等列表更新后重试。",
    host_unavailable: "无法连接 Ghost，请等列表自动更新后确认操作结果。"
};

let profiles = [];
let nodes = [];
let editing = null;
let detectedPath = "";
let busy = false;
let dataLoaded = false;
let syncing = false;
let revision = 0;

async function call(body) {
    const response = await fetch("./api", {
        method: "POST",
        headers: { "Content-Type": "application/json", "X-Chrome-Launcher": "1" },
        body: JSON.stringify(body)
    });
    const result = await response.json();
    if (!response.ok || result.status !== "ok") {
        throw new Error(errors[result.error] || `Ghost 返回错误（${result.error || response.status}）。`);
    }
    return result;
}

function nodeName(id) {
    if (id === "direct") return "直连";
    const match = nodes.find(item => item.id === id);
    return match ? (match.name || match.id) : `节点不可用 · ${id}`;
}

function showNodes(selection) {
    node.replaceChildren(new Option("请选择", ""), new Option("直连", "direct"));
    for (const item of nodes) node.add(new Option(item.name || item.id, item.id));
    if (selection && !Array.from(node.options).some(option => option.value === selection)) {
        const missing = new Option(nodeName(selection), selection);
        missing.disabled = true;
        node.add(missing);
    }
    node.value = selection;
}

function button(label, onClick, disabled, className = "") {
    const element = document.createElement("button");
    element.type = "button";
    element.textContent = label;
    element.className = className;
    element.disabled = busy || disabled;
    element.addEventListener("click", onClick);
    return element;
}

function renderProfiles() {
    const list = byId("profiles");
    list.replaceChildren();
    byId("columns").hidden = profiles.length === 0;
    const runningCount = profiles.filter(profile => profile.status === "running").length;
    byId("summary").textContent = dataLoaded ? `${profiles.length} 个实例 · ${runningCount} 个运行中` : "";
    if (!profiles.length) {
        if (dataLoaded) {
            const empty = document.createElement("li");
            empty.className = "empty";
            empty.textContent = "暂无实例。点击「添加实例」配置一个 Chrome。";
            list.append(empty);
        }
        return;
    }
    for (const profile of profiles) {
        const item = document.createElement("li");
        item.className = "profile";
        const name = document.createElement("strong");
        const path = document.createElement("span");
        const upstream = document.createElement("span");
        const state = document.createElement("span");
        const running = profile.status === "running";
        name.textContent = profile.name;
        path.className = "data-path";
        path.dataset.label = "用户数据目录";
        path.textContent = profile.dataDir;
        upstream.className = "upstream";
        upstream.dataset.label = "上游节点";
        upstream.textContent = nodeName(profile.proxyNodeId);
        state.className = running ? "state running" : "state";
        state.textContent = running ? "运行中" : "已停止";
        const actions = document.createElement("div");
        actions.className = "actions";
        actions.append(button("编辑", () => edit(profile), running));
        if (running) actions.append(button("停止", () => control(profile, false), false, "stop"));
        else actions.append(button("启动", () => control(profile, true), false, "start"));
        item.append(name, path, upstream, state, actions);
        list.append(item);
    }
}

function edit(profile) {
    editing = profile ? profile.targetId : null;
    byId("form-title").textContent = editing ? "编辑实例" : "添加实例";
    byId("executable-field").hidden = Boolean(editing);
    chromePath.required = !editing;
    chromePath.value = detectedPath;
    byId("chrome-path-hint").textContent = detectedPath
        ? "从本机常见安装目录检测到，可手动修改。"
        : "常见安装目录未找到 Chrome，请填写 chrome.exe 的完整路径。";
    byId("profile-name").value = profile?.name || "";
    byId("data-dir").value = profile?.dataDir || "";
    showNodes(profile?.proxyNodeId || "");
    formStatus.textContent = "";
    formStatus.className = "";
    dialog.showModal();
}

function setBusy(value) {
    busy = value;
    byId("add").disabled = value || !dataLoaded;
    for (const control of form.elements) control.disabled = value || !dataLoaded;
    renderProfiles();
}

function applyList(result) {
    const nodesChanged = !dataLoaded || JSON.stringify(nodes) !== JSON.stringify(result.nodes);
    profiles = result.profiles;
    nodes = result.nodes;
    dataLoaded = true;
    detectedPath = result.chromePath;
    if (nodesChanged) showNodes(node.value);
}

async function refresh() {
    applyList(await call({ action: "list" }));
}

async function syncVisibleList() {
    if (document.hidden || busy || syncing) return;
    syncing = true;
    const startedAt = revision;
    try {
        const result = await call({ action: "list" });
        if (startedAt !== revision) return;
        const firstLoad = !dataLoaded;
        const changed = JSON.stringify(profiles) !== JSON.stringify(result.profiles) ||
                        JSON.stringify(nodes) !== JSON.stringify(result.nodes);
        applyList(result);
        if (firstLoad) {
            status.textContent = "";
            status.className = "";
            setBusy(false);
        } else if (changed) {
            renderProfiles();
        }
    } catch {
        if (!dataLoaded && startedAt === revision) {
            status.textContent = "读取实例失败，正在自动重试…";
            status.className = "error";
        }
    } finally {
        syncing = false;
    }
}

async function run(operation, waiting = "", message = status) {
    if (busy) return;
    revision++;
    setBusy(true);
    message.textContent = waiting;
    message.className = "";
    try {
        await operation();
    } catch (error) {
        message.textContent = error instanceof TypeError ? errors.host_unavailable : error.message;
        message.className = "error";
    } finally {
        setBusy(false);
    }
}

function control(profile, launch) {
    run(async () => {
        await call({ action: launch ? "launch" : "stop", targetId: profile.targetId });
        await refresh();
        status.textContent = launch
            ? `已向 Ghost 提交「${profile.name}」的启动。`
            : `已向 Ghost 提交「${profile.name}」的停止。`;
    }, launch ? "正在启动 Chrome。如 Ghost 弹出确认窗口，请确认启动。" : "正在停止 Chrome…");
}

byId("add").addEventListener("click", () => edit(null));
byId("cancel").addEventListener("click", () => dialog.close());
dialog.addEventListener("cancel", event => {
    if (busy) event.preventDefault();
});
form.addEventListener("submit", event => {
    event.preventDefault();
    const body = {
        action: editing ? "update" : "create",
        name: byId("profile-name").value.trim(),
        dataDir: byId("data-dir").value.trim(),
        proxyNodeId: node.value
    };
    if (editing) body.targetId = editing;
    else body.chromePath = chromePath.value.trim();
    run(async () => {
        await call(body);
        dialog.close();
        status.className = "";
        status.textContent = "配置已保存。正在刷新…";
        await refresh();
        status.textContent = "配置已保存。";
    }, "正在保存…", formStatus).then(() => {
        if (!dialog.open && formStatus.className === "error") {
            status.textContent = `配置已保存。刷新失败，${formStatus.textContent}`;
            status.className = "error";
        } else if (!dialog.open) {
            byId("add").focus();
        }
    });
});

showNodes("");
setBusy(false);
status.textContent = "正在读取实例和节点…";
syncVisibleList();
window.setInterval(syncVisibleList, 8000);
document.addEventListener("visibilitychange", syncVisibleList);
