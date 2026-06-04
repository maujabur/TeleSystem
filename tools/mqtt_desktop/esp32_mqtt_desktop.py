#!/usr/bin/env python3
import json
import queue
import ssl
import tkinter as tk
import uuid
from dataclasses import dataclass, field
from datetime import datetime, timedelta
from pathlib import Path
from typing import Any, Dict, Optional, Tuple
from urllib.parse import urlparse

import customtkinter as ctk
import paho.mqtt.client as mqtt
from tkinter import END, StringVar, TclError, filedialog
from tkinter import ttk


BASE_TOPIC = "v1/acr"
TRIGGER_MODE_LABEL_TO_VALUE = {
    "0 - Prediction": 0,
    "1 - Probabilidade": 1,
    "2 - Prediction OU probabilidade": 2,
    "3 - Prediction E probabilidade": 3,
}
TRIGGER_MODE_VALUE_TO_LABEL = {v: k for k, v in TRIGGER_MODE_LABEL_TO_VALUE.items()}

APSTA_POLICY_LABEL_TO_VALUE = {
    "0 - Always on": 0,
    "1 - Auto timeout": 1,
    "2 - STA only": 2,
}
APSTA_POLICY_VALUE_TO_LABEL = {v: k for k, v in APSTA_POLICY_LABEL_TO_VALUE.items()}

TOPICS_TO_SUBSCRIBE = [
    f"{BASE_TOPIC}/+/status",
    f"{BASE_TOPIC}/+/heartbeat",
    f"{BASE_TOPIC}/+/state",
    f"{BASE_TOPIC}/+/event",
    f"{BASE_TOPIC}/+/cmd/out",
]
PRESENCE_MESSAGE_TYPES = {"status", "heartbeat", "state", "event"}
PENDING_COMMAND_TIMEOUT_SEC = 30
MAX_LOG_LINES = 2000


@dataclass
class MessageSnapshot:
    timestamp: datetime
    topic: str
    payload_obj: Optional[Dict[str, Any]]
    payload_raw: str


@dataclass
class PendingCommand:
    device_id: str
    command: str
    sent_at: datetime


@dataclass
class DeviceInfo:
    device_id: str
    online: bool = False
    seen_live: bool = False
    last_probe_at: Optional[datetime] = None
    last_seen: Optional[datetime] = None
    last_get_state_result: Optional[Dict[str, Any]] = None
    last_get_state_at: Optional[datetime] = None
    last_technical_status_result: Optional[Dict[str, Any]] = None
    last_technical_status_at: Optional[datetime] = None
    fw: str = "-"
    session_id: str = "-"
    last_messages: Dict[str, MessageSnapshot] = field(default_factory=dict)


class MQTTManager:
    def __init__(self, event_queue: queue.Queue):
        self.event_queue = event_queue
        self.client: Optional[mqtt.Client] = None
        self.connected = False

    def connect(
        self,
        host: str,
        port: int,
        username: str,
        password: str,
        tls_enabled: bool,
    ) -> None:
        if self.client is not None:
            self.disconnect()

        client = mqtt.Client(mqtt.CallbackAPIVersion.VERSION2)
        if username:
            client.username_pw_set(username=username, password=password)

        if tls_enabled:
            client.tls_set(cert_reqs=ssl.CERT_REQUIRED)

        client.on_connect = self._on_connect
        client.on_disconnect = self._on_disconnect
        client.on_message = self._on_message

        try:
            client.connect(host, port, keepalive=60)
            client.loop_start()
            self.client = client
            self.event_queue.put(("log", {"level": "info", "text": f"Connecting to {host}:{port}..."}))
        except Exception as exc:
            self.event_queue.put(("error", f"Connection failed: {exc}"))
            self.event_queue.put(("connection", {"connected": False, "reason": "Connect failed"}))

    def disconnect(self) -> None:
        if self.client is None:
            return

        try:
            self.client.loop_stop()
            self.client.disconnect()
        except Exception:
            pass
        finally:
            self.client = None
            self.connected = False
            self.event_queue.put(("connection", {"connected": False, "reason": "Disconnected"}))

    def publish_command(self, device_id: str, payload: Dict[str, Any]) -> Tuple[bool, str]:
        if not self.client or not self.connected:
            return False, "Not connected"

        topic = f"{BASE_TOPIC}/{device_id}/cmd/in"
        try:
            payload_str = json.dumps(payload, ensure_ascii=True)
            result = self.client.publish(topic, payload_str, qos=1)
            if result.rc == mqtt.MQTT_ERR_SUCCESS:
                self.event_queue.put(("log", {"level": "info", "text": f"Published cmd to {topic}: {payload_str}"}))
                return True, "OK"
            return False, f"Publish failed with rc={result.rc}"
        except Exception as exc:
            return False, f"Publish failed: {exc}"

    def clear_retained_topics(self, topics: list[str]) -> Tuple[int, int]:
        if not self.client or not self.connected:
            return 0, len(topics)

        ok_count = 0
        fail_count = 0
        for topic in topics:
            try:
                result = self.client.publish(topic, payload="", qos=1, retain=True)
                if result.rc == mqtt.MQTT_ERR_SUCCESS:
                    ok_count += 1
                else:
                    fail_count += 1
            except Exception:
                fail_count += 1
        return ok_count, fail_count

    def _on_connect(self, client, userdata, flags, reason_code, properties):
        self.connected = reason_code == 0
        if self.connected:
            for topic in TOPICS_TO_SUBSCRIBE:
                client.subscribe(topic, qos=1)
            self.event_queue.put(("connection", {"connected": True, "reason": "Connected"}))
            self.event_queue.put(("log", {"level": "info", "text": "Connected and subscribed to wildcard topics"}))
        else:
            self.event_queue.put(("connection", {"connected": False, "reason": f"Connect failed (code={reason_code})"}))

    def _on_disconnect(self, client, userdata, flags, reason_code, properties):
        self.connected = False
        self.event_queue.put(("connection", {"connected": False, "reason": f"Disconnected (code={reason_code})"}))

    def _on_message(self, client, userdata, msg):
        payload_raw = msg.payload.decode("utf-8", errors="replace")
        payload_obj = None
        if payload_raw.strip():
            try:
                maybe_obj = json.loads(payload_raw)
                if isinstance(maybe_obj, dict):
                    payload_obj = maybe_obj
            except json.JSONDecodeError:
                self.event_queue.put(("log", {"level": "warn", "text": f"Invalid JSON on {msg.topic}"}))

        self.event_queue.put(
            (
                "message",
                {
                    "topic": msg.topic,
                    "payload_raw": payload_raw,
                    "payload_obj": payload_obj,
                    "retained": bool(msg.retain),
                    "timestamp": datetime.now(),
                },
            )
        )


class App(ctk.CTk):
    def __init__(self):
        super().__init__()
        self.title("ACR ESP32 MQTT Desktop MVP")
        self.geometry("1400x820")

        ctk.set_appearance_mode("system")
        ctk.set_default_color_theme("blue")

        self.event_queue: queue.Queue = queue.Queue()
        self.mqtt = MQTTManager(self.event_queue)
        self.devices: Dict[str, DeviceInfo] = {}
        self.selected_device: Optional[str] = None
        self.detail_value_labels: Dict[str, ctk.CTkLabel] = {}
        self.status_value_labels: Dict[str, ctk.CTkLabel] = {}
        self.status_display_values: Dict[str, str] = {}
        self.status_raw_text_value = ""
        self.settings_vars: Dict[str, Any] = {}
        self.settings_inputs: Dict[str, Any] = {}
        self.settings_snapshot: Dict[str, Any] = {}
        self.settings_loaded = False
        self.settings_loaded_device_id: Optional[str] = None
        self.settings_save_queue: list[tuple[str, Dict[str, Any]]] = []
        self.settings_save_device_id: Optional[str] = None
        self.settings_save_total = 0
        self.settings_save_in_progress = False
        self.pending_cmd_by_id: Dict[str, PendingCommand] = {}
        self.log_line_count = 0
        self.mousewheel_scroll_frames: list[Any] = []
        self.tree_sort_column = "device_id"
        self.tree_sort_desc = False
        self.tree_heading_labels: Dict[str, str] = {}
        self.status_icons: Dict[str, tk.PhotoImage] = {}
        self.device_tree_refresh_after_id: Optional[str] = None
        self.auto_connect_pending = False
        self.device_context_menu: Optional[ctk.CTkToplevel] = None
        self.tooltip_window: Optional[ctk.CTkToplevel] = None
        self.tooltip_after_id: Optional[str] = None
        self.tooltip_widget: Optional[Any] = None
        self.tooltip_tree_cell: Optional[Tuple[str, str]] = None

        self.host_var = StringVar(value="localhost")
        self.port_var = StringVar(value="1883")
        self.user_var = StringVar(value="")
        self.pass_var = StringVar(value="")
        self.clear_retained_device_var = StringVar(value="")
        self.device_search_var = StringVar(value="")
        self.device_filter_var = StringVar(value="Todos")
        self.device_counts_var = StringVar(value="Dispositivos: 0 | Online: 0 | Offline: 0 | Triagem: 0 | Retained: 0")
        self.tls_var = ctk.BooleanVar(value=False)
        self.auto_probe_var = ctk.BooleanVar(value=True)
        self.heartbeat_timeout_var = StringVar(value="180")
        self.auto_connect_on_start_var = ctk.BooleanVar(value=False)
        self.technical_auto_update_var = ctk.BooleanVar(value=True)
        self.technical_update_interval_var = StringVar(value="3")
        self.conn_state_var = StringVar(value="Starting...")

        self._build_ui()
        self._enable_mousewheel_scrolling()
        self._load_example_or_local_config()
        if bool(self.auto_connect_on_start_var.get()):
            self._set_auto_connect_visual_state()
        else:
            self._set_disconnected_visual_state("Disconnected")

        self.after(100, self._drain_events)
        self.after(1000, self._check_offline_devices)
        self.after(1000, self._refresh_device_age_tick)
        self.after(1000, self._technical_status_auto_update_tick)
        self.after(250, self._auto_connect_if_configured)

    def _create_status_icon(self, fill_color: str, border_color: str) -> tk.PhotoImage:
        size = 14
        radius = 5.5
        center = (size - 1) / 2
        image = tk.PhotoImage(width=size, height=size)
        for y in range(size):
            for x in range(size):
                distance = ((x - center) ** 2 + (y - center) ** 2) ** 0.5
                if distance <= radius:
                    color = border_color if distance >= radius - 1.3 else fill_color
                    image.put(color, (x, y))
        return image

    def _build_ui(self):
        self.grid_columnconfigure(0, weight=0)
        self.grid_columnconfigure(1, weight=1)
        self.grid_rowconfigure(0, weight=1)

        left = ctk.CTkFrame(self, width=430)
        left.grid(row=0, column=0, sticky="nsew", padx=12, pady=12)
        left.grid_columnconfigure(0, weight=1)
        left.grid_rowconfigure(2, weight=1)

        conn = ctk.CTkFrame(left)
        conn.grid(row=0, column=0, sticky="ew", padx=10, pady=(10, 8))
        conn.grid_columnconfigure(1, weight=1)

        ctk.CTkLabel(conn, text="MQTT Connection", font=ctk.CTkFont(size=16, weight="bold")).grid(
            row=0, column=0, columnspan=2, sticky="w", padx=8, pady=(8, 10)
        )

        self._labeled_entry(conn, 1, "Host", self.host_var)
        self._labeled_entry(conn, 2, "Port", self.port_var)
        self._labeled_entry(conn, 3, "User", self.user_var)
        self._labeled_entry(conn, 4, "Password", self.pass_var, show="*")
        self._labeled_entry(conn, 5, "HB timeout (s)", self.heartbeat_timeout_var)

        ctk.CTkCheckBox(conn, text="TLS", variable=self.tls_var).grid(row=6, column=0, sticky="w", padx=8, pady=(2, 8))
        ctk.CTkCheckBox(conn, text="Auto-probe online", variable=self.auto_probe_var).grid(
            row=6, column=1, sticky="w", padx=8, pady=(2, 8)
        )

        btns = ctk.CTkFrame(conn, fg_color="transparent")
        btns.grid(row=7, column=0, columnspan=2, sticky="ew", padx=8, pady=(0, 8))
        btns.grid_columnconfigure((0, 1), weight=1)
        self.btn_connect = ctk.CTkButton(btns, text="Connect", command=self._connect)
        self.btn_connect.grid(row=0, column=0, sticky="ew", padx=(0, 4))
        self.btn_disconnect = ctk.CTkButton(btns, text="Disconnect", command=self._disconnect, state="disabled")
        self.btn_disconnect.grid(row=0, column=1, sticky="ew", padx=(4, 0))

        status_frame = ctk.CTkFrame(conn)
        status_frame.grid(row=8, column=0, columnspan=2, sticky="ew", padx=8, pady=(0, 8))
        self.conn_indicator = ctk.CTkLabel(status_frame, text=" ", width=18, height=18, fg_color="#6c757d", corner_radius=8)
        self.conn_indicator.grid(row=0, column=0, padx=(6, 8), pady=6)
        ctk.CTkLabel(status_frame, textvariable=self.conn_state_var).grid(row=0, column=1, sticky="w")
        self._set_connection_ui_state("starting")

        devices_frame = ctk.CTkFrame(left)
        devices_frame.grid(row=2, column=0, sticky="nsew", padx=10, pady=(6, 10))
        devices_frame.grid_columnconfigure(0, weight=1)
        devices_frame.grid_rowconfigure(2, weight=1)

        ctk.CTkLabel(devices_frame, text="Dispositivos", font=ctk.CTkFont(size=16, weight="bold")).grid(
            row=0, column=0, sticky="w", padx=8, pady=(8, 4)
        )

        device_tools = ctk.CTkFrame(devices_frame, fg_color="transparent")
        device_tools.grid(row=1, column=0, sticky="ew", padx=8, pady=(0, 6))
        device_tools.grid_columnconfigure(0, weight=1)
        device_tools.grid_columnconfigure(1, weight=0)
        device_tools.grid_columnconfigure(2, weight=0)
        ctk.CTkEntry(
            device_tools,
            textvariable=self.device_search_var,
            placeholder_text="Buscar device, fw, sessao ou status",
        ).grid(row=0, column=0, sticky="ew", padx=(0, 6))
        ctk.CTkOptionMenu(
            device_tools,
            variable=self.device_filter_var,
            values=["Todos", "Online", "Offline", "Triagem", "Retained"],
            width=116,
        ).grid(row=0, column=1, sticky="e", padx=(0, 6))
        self.btn_ping_all = ctk.CTkButton(
            device_tools,
            text="⟳",
            width=32,
            height=28,
            command=self._ping_all_devices,
        )
        self.btn_ping_all.grid(row=0, column=2, sticky="e")
        self._bind_delayed_tooltip(self.btn_ping_all, lambda: "Enviar ping para todos os dispositivos conhecidos")
        ctk.CTkLabel(
            device_tools,
            textvariable=self.device_counts_var,
            anchor="w",
            text_color=("gray40", "gray70"),
        ).grid(row=1, column=0, columnspan=3, sticky="ew", pady=(4, 0))

        self.device_search_var.trace_add("write", self._on_device_filter_changed)
        self.device_filter_var.trace_add("write", self._on_device_filter_changed)

        cols = ("presence", "device_id", "age", "fw", "summary")
        self.tree = ttk.Treeview(devices_frame, columns=cols, show=("tree", "headings"), height=12)
        self.tree.grid(row=2, column=0, sticky="nsew", padx=8, pady=(0, 8))
        self.tree.bind("<<TreeviewSelect>>", self._on_select_device)
        self.tree.bind("<Button-3>", self._show_device_context_menu)
        self.tree.bind("<Button-2>", self._show_device_context_menu)
        self.tree.bind("<Motion>", self._on_device_tree_motion, add="+")
        self.tree.bind("<Leave>", self._hide_tooltip, add="+")
        self.tree.tag_configure("online", foreground="#1f8b24")
        self.tree.tag_configure("offline", foreground="#d11a2a")
        self.tree.tag_configure("retained", foreground="#6c757d")
        self.tree.tag_configure("attention", foreground="#d67a00")
        self.tree.tag_configure("pending", foreground="#0b5ed7")
        self.status_icons = {
            "online": self._create_status_icon("#25a83a", "#166b24"),
            "offline": self._create_status_icon("#d11a2a", "#85101b"),
            "retained": self._create_status_icon("#8a8f98", "#5e646d"),
            "attention": self._create_status_icon("#d67a00", "#8a4f00"),
            "pending": self._create_status_icon("#0b5ed7", "#063b83"),
        }
        self.tree.heading("#0", text="")
        self.tree.column("#0", width=28, minwidth=28, stretch=False, anchor="center")

        widths = {"presence": 78, "device_id": 142, "age": 82, "fw": 72, "summary": 300}
        heading_labels = {
            "device_id": "device_id",
            "presence": "estado",
            "age": "idade",
            "fw": "fw",
            "summary": "resumo",
        }
        self.tree_heading_labels = heading_labels
        for col in cols:
            self.tree.heading(col, text=heading_labels.get(col, col), command=lambda c=col: self._on_tree_heading_click(c))
            self.tree.column(col, width=widths[col], anchor="w")
        self._refresh_tree_headings()

        right = ctk.CTkFrame(self)
        right.grid(row=0, column=1, sticky="nsew", padx=(0, 12), pady=12)
        right.grid_columnconfigure(0, weight=1)
        right.grid_rowconfigure(0, weight=1)

        split = ttk.Panedwindow(right, orient=tk.VERTICAL)
        split.grid(row=0, column=0, sticky="nsew", padx=10, pady=10)

        top_container = ctk.CTkFrame(right, fg_color="transparent")
        bottom_container = ctk.CTkFrame(right)
        split.add(top_container, weight=4)
        split.add(bottom_container, weight=1)

        top_container.grid_columnconfigure(0, weight=1)
        top_container.grid_rowconfigure(0, weight=1)
        bottom_container.grid_columnconfigure(0, weight=1)
        bottom_container.grid_rowconfigure(0, weight=1)

        self.tabs = ctk.CTkTabview(top_container, command=self._on_main_tab_changed)
        tabs = self.tabs
        tabs.grid(row=0, column=0, sticky="nsew")
        tabs.add("Resumo")
        tabs.add("Status")
        tabs.add("Comandos")
        tabs.add("Settings")

        tab_overview = tabs.tab("Resumo")
        tab_overview.grid_columnconfigure(0, weight=1)
        tab_overview.grid_rowconfigure(0, weight=1)
        self.details = ctk.CTkScrollableFrame(tab_overview)
        self.details.grid(row=0, column=0, sticky="nsew", padx=8, pady=8)
        self.details.grid_columnconfigure(0, weight=0)
        self.details.grid_columnconfigure(1, weight=1)
        self.mousewheel_scroll_frames.append(self.details)
        self._build_details_panel()

        tab_status = tabs.tab("Status")
        tab_status.grid_columnconfigure(0, weight=1)
        tab_status.grid_rowconfigure(0, weight=1)
        self._build_status_panel(tab_status)

        tab_commands = tabs.tab("Comandos")
        tab_commands.grid_columnconfigure(0, weight=1)
        self._build_commands_panel(tab_commands)

        tab_settings = tabs.tab("Settings")
        tab_settings.grid_columnconfigure(0, weight=1)
        tab_settings.grid_rowconfigure(0, weight=1)
        self._build_settings_panel(tab_settings)

        self.log_box = ctk.CTkTextbox(bottom_container, wrap="none")
        self.log_box.grid(row=0, column=0, sticky="nsew", padx=4, pady=4)
        self.log_box.tag_config("warn", foreground="#d67a00")
        self.log_box.tag_config("error", foreground="#d11a2a")
        self.log_box.tag_config("selected_cmd", foreground="#0b5ed7")
        self.log_context_menu: Optional[ctk.CTkToplevel] = None
        self.log_box.bind("<Button-3>", self._show_log_context_menu)
        self.log_box.bind("<Button-2>", self._show_log_context_menu)
        self.bind_all("<Button-1>", self._hide_log_context_menu_if_outside, add="+")
        self.bind_all("<Button-1>", self._hide_device_context_menu_if_outside, add="+")
        self.bind_all("<Escape>", self._hide_log_context_menu, add="+")
        self.bind_all("<Escape>", self._hide_device_context_menu, add="+")

        self._append_log("Application started", tag="info")

    def _build_commands_panel(self, parent):
        parent.grid_rowconfigure(1, weight=1)
        parent.grid_columnconfigure(0, weight=1)

        frame = ctk.CTkFrame(parent)
        frame.grid(row=0, column=0, sticky="ew", padx=8, pady=8)
        frame.grid_columnconfigure(0, weight=1)

        ctk.CTkLabel(frame, text="Comandos rapidos", font=ctk.CTkFont(size=16, weight="bold")).grid(
            row=0, column=0, sticky="w", padx=8, pady=(4, 6)
        )

        row_a = ctk.CTkFrame(frame)
        row_a.grid(row=1, column=0, sticky="ew", padx=8, pady=(0, 8))
        row_a.grid_columnconfigure((0, 1, 2, 3), weight=1)
        ctk.CTkButton(row_a, text="ping", command=lambda: self._send_cmd("ping")).grid(row=0, column=0, sticky="ew", padx=4, pady=4)
        ctk.CTkButton(row_a, text="get_state", command=lambda: self._send_cmd("get_state")).grid(row=0, column=1, sticky="ew", padx=4, pady=4)
        ctk.CTkButton(row_a, text="get_settings", command=self._request_get_settings).grid(row=0, column=2, sticky="ew", padx=4, pady=4)
        ctk.CTkButton(row_a, text="status_tecnico", command=lambda: self._send_cmd("get_technical_status")).grid(
            row=0, column=3, sticky="ew", padx=4, pady=4
        )

        row_b = ctk.CTkFrame(frame)
        row_b.grid(row=2, column=0, sticky="ew", padx=8, pady=(0, 8))
        row_b.grid_columnconfigure((0, 1), weight=1)
        ctk.CTkButton(row_b, text="apply_and_reboot", command=lambda: self._send_cmd("apply_and_reboot")).grid(
            row=0, column=0, sticky="ew", padx=4, pady=4
        )

        hb_frame = ctk.CTkFrame(row_b, fg_color="transparent")
        hb_frame.grid(row=0, column=1, sticky="ew", padx=4, pady=4)
        hb_frame.grid_columnconfigure(0, weight=1)
        self.hb_interval_var = StringVar(value="60")
        hb_entry = ctk.CTkEntry(hb_frame, textvariable=self.hb_interval_var)
        hb_entry.grid(row=0, column=0, sticky="ew", pady=(0, 4))
        self.settings_inputs["heartbeat_interval_s"] = hb_entry
        ctk.CTkButton(hb_frame, text="set_heartbeat_interval", command=self._send_set_heartbeat).grid(
            row=1, column=0, sticky="ew"
        )

        row_c = ctk.CTkFrame(frame)
        row_c.grid(row=3, column=0, sticky="ew", padx=8, pady=(0, 8))
        row_c.grid_columnconfigure(0, weight=1)
        row_c.grid_columnconfigure(1, weight=0)
        ctk.CTkEntry(
            row_c,
            textvariable=self.clear_retained_device_var,
            placeholder_text="device_id para limpar retained (vazio usa selecionado)",
        ).grid(row=0, column=0, sticky="ew", padx=4, pady=4)
        ctk.CTkButton(row_c, text="Limpar retained", command=self._clear_retained_for_device).grid(
            row=0, column=1, sticky="ew", padx=4, pady=4
        )

        info = ctk.CTkTextbox(parent, height=120)
        info.grid(row=1, column=0, sticky="nsew", padx=8, pady=(0, 8))
        info.insert(
            END,
            "Comandos desta aba sao enviados para o dispositivo selecionado na lista.\n"
            "- use get_state para atualizar a aba Status/State\n"
            "- use status_tecnico para consultar os dados da pagina de status tecnico\n"
            "- use get_settings para preencher Settings\n"
            "- Limpar retained remove snapshots antigos do broker\n",
        )
        info.configure(state="disabled")

    def _build_settings_panel(self, parent):
        parent.grid_columnconfigure(0, weight=1)
        parent.grid_rowconfigure(0, weight=0)
        parent.grid_rowconfigure(1, weight=0)
        parent.grid_rowconfigure(2, weight=1)

        self.settings_vars = {
            "automatic_enabled": ctk.BooleanVar(value=False),
            "automatic_interval_ms": StringVar(value=""),
            "capture_duration_seconds": StringVar(value=""),
            "digital_gain": StringVar(value=""),
            "silence_threshold_rms": StringVar(value=""),
            "silence_hysteresis_rms": StringVar(value=""),
            "min_active_ms": StringVar(value=""),
            "trigger_mode_label": StringVar(value=""),
            "ai_probability_threshold": StringVar(value=""),
            "trigger_enabled": ctk.BooleanVar(value=False),
            "trigger_gpio": StringVar(value=""),
            "trigger_active_level": StringVar(value=""),
            "trigger_pulse_ms": StringVar(value=""),
            "provisioning_ssid": StringVar(value=""),
            "sta_max_retry": StringVar(value=""),
            "apsta_policy_label": StringVar(value=""),
            "apsta_grace_period_s": StringVar(value=""),
            "acr_region": StringVar(value=""),
            "acr_container_id": StringVar(value=""),
            "acr_upload_prefix": StringVar(value=""),
            "acr_bearer_token": StringVar(value=""),
        }
        self.settings_apply_identity_var = ctk.BooleanVar(value=False)
        self.settings_inputs = {}

        actions = ctk.CTkFrame(parent)
        actions.grid(row=0, column=0, sticky="ew", padx=8, pady=(8, 4))
        actions.grid_columnconfigure(0, weight=0)
        actions.grid_columnconfigure(1, weight=0)
        actions.grid_columnconfigure(2, weight=1)
        actions.grid_columnconfigure(3, weight=0)
        actions.grid_columnconfigure(4, weight=0)
        actions.grid_columnconfigure(5, weight=0)
        ctk.CTkButton(actions, text="Ler settings", command=self._request_get_settings).grid(
            row=0, column=0, sticky="ew", padx=4, pady=4
        )
        self.btn_save_settings = ctk.CTkButton(actions, text="Salvar settings", command=self._send_set_settings)
        self.btn_save_settings.grid(
            row=0, column=1, sticky="ew", padx=4, pady=4
        )
        ctk.CTkButton(actions, text="Salvar heartbeat", command=self._send_set_heartbeat).grid(
            row=0, column=3, sticky="ew", padx=4, pady=4
        )
        ctk.CTkButton(actions, text="Apply + reboot", command=lambda: self._send_cmd("apply_and_reboot")).grid(
            row=0, column=4, sticky="ew", padx=4, pady=4
        )
        profile_actions = ctk.CTkFrame(actions, fg_color="transparent")
        profile_actions.grid(row=0, column=5, sticky="e", padx=4, pady=4)
        profile_actions.grid_columnconfigure((1, 2), weight=0)
        apply_identity_check = ctk.CTkCheckBox(
            profile_actions,
            text="identidade",
            variable=self.settings_apply_identity_var,
            width=92,
        )
        apply_identity_check.grid(row=0, column=0, sticky="w", padx=(0, 8))
        self.btn_settings_file_open = ctk.CTkButton(
            profile_actions,
            text="Abrir perfil",
            width=112,
            command=self._load_settings_file,
        )
        self.btn_settings_file_open.grid(row=0, column=1, sticky="ew", padx=(0, 4))
        self.btn_settings_file_save = ctk.CTkButton(
            profile_actions,
            text="Salvar perfil",
            width=112,
            command=self._save_settings_file,
        )
        self.btn_settings_file_save.grid(row=0, column=2, sticky="ew")
        self._bind_delayed_tooltip(
            apply_identity_check,
            lambda: "Quando marcado, Abrir perfil tambem aplica provisioning_ssid e upload_prefix",
        )
        self._bind_delayed_tooltip(self.btn_settings_file_open, lambda: "Abrir perfil JSON de settings no formulario")
        self._bind_delayed_tooltip(self.btn_settings_file_save, lambda: "Salvar perfil JSON com o snapshot atual de settings")

        self.settings_status_label = ctk.CTkLabel(parent, text="Ultima leitura: - | leia os settings antes de salvar", anchor="w")
        self.settings_status_label.grid(row=1, column=0, sticky="ew", padx=8, pady=(0, 4))
        self.btn_save_settings.configure(state="disabled")
        self.btn_settings_file_save.configure(state="disabled")

        scroll = ctk.CTkScrollableFrame(parent)
        scroll.grid(row=2, column=0, sticky="nsew", padx=8, pady=(0, 8))
        scroll.grid_columnconfigure(0, weight=1)
        self.mousewheel_scroll_frames.append(scroll)

        row = 0
        row = self._build_settings_section_acr_control(scroll, row)
        row = self._build_settings_section_ai(scroll, row)
        row = self._build_settings_section_connectivity(scroll, row)
        row = self._build_settings_section_trigger_output(scroll, row)
        self._build_settings_section_acr_cloud(scroll, row)

    def _build_settings_section_acr_control(self, parent, row_start: int) -> int:
        frame = ctk.CTkFrame(parent)
        frame.grid(row=row_start, column=0, sticky="ew", pady=(0, 8))
        frame.grid_columnconfigure(1, weight=1)
        frame.grid_columnconfigure(3, weight=1)
        ctk.CTkLabel(frame, text="Loop automatico e audio", font=ctk.CTkFont(size=14, weight="bold")).grid(
            row=0, column=0, columnspan=4, sticky="w", padx=8, pady=(8, 6)
        )

        ctk.CTkCheckBox(frame, text="automatic_enabled", variable=self.settings_vars["automatic_enabled"]).grid(
            row=1, column=0, sticky="w", padx=8, pady=4
        )
        self._settings_entry(frame, 1, 2, "automatic_interval_ms", self.settings_vars["automatic_interval_ms"], key_name="automatic_interval_ms")
        self._settings_entry(frame, 2, 0, "capture_duration_seconds", self.settings_vars["capture_duration_seconds"], key_name="capture_duration_seconds")
        self._settings_entry(frame, 2, 2, "digital_gain", self.settings_vars["digital_gain"], key_name="digital_gain")
        self._settings_entry(frame, 3, 0, "silence_threshold_rms", self.settings_vars["silence_threshold_rms"], key_name="silence_threshold_rms")
        self._settings_entry(frame, 3, 2, "silence_hysteresis_rms", self.settings_vars["silence_hysteresis_rms"], key_name="silence_hysteresis_rms")
        self._settings_entry(frame, 4, 0, "min_active_ms", self.settings_vars["min_active_ms"], key_name="min_active_ms")
        self._settings_entry(frame, 4, 2, "heartbeat_interval_s", self.hb_interval_var, key_name="heartbeat_interval_s")
        return row_start + 1

    def _build_settings_section_ai(self, parent, row_start: int) -> int:
        frame = ctk.CTkFrame(parent)
        frame.grid(row=row_start, column=0, sticky="ew", pady=(0, 8))
        frame.grid_columnconfigure(1, weight=1)
        frame.grid_columnconfigure(3, weight=1)
        ctk.CTkLabel(frame, text="Politica IA", font=ctk.CTkFont(size=14, weight="bold")).grid(
            row=0, column=0, columnspan=4, sticky="w", padx=8, pady=(8, 6)
        )
        ctk.CTkLabel(frame, text="trigger_mode").grid(row=1, column=0, sticky="w", padx=8, pady=4)
        trigger_mode_select = ctk.CTkOptionMenu(
            frame,
            variable=self.settings_vars["trigger_mode_label"],
            values=list(TRIGGER_MODE_LABEL_TO_VALUE.keys()),
        )
        trigger_mode_select.grid(row=1, column=1, sticky="ew", padx=8, pady=4)
        self.settings_inputs["trigger_mode"] = trigger_mode_select
        self._settings_entry(frame, 1, 2, "ai_probability_threshold", self.settings_vars["ai_probability_threshold"], key_name="ai_probability_threshold")
        return row_start + 1

    def _build_settings_section_connectivity(self, parent, row_start: int) -> int:
        frame = ctk.CTkFrame(parent)
        frame.grid(row=row_start, column=0, sticky="ew", pady=(0, 8))
        frame.grid_columnconfigure(1, weight=1)
        frame.grid_columnconfigure(3, weight=1)
        ctk.CTkLabel(frame, text="Conectividade", font=ctk.CTkFont(size=14, weight="bold")).grid(
            row=0, column=0, columnspan=4, sticky="w", padx=8, pady=(8, 6)
        )
        self._settings_entry(frame, 1, 0, "provisioning_ssid", self.settings_vars["provisioning_ssid"], key_name="provisioning_ssid")
        self._settings_entry(frame, 1, 2, "sta_max_retry", self.settings_vars["sta_max_retry"], key_name="sta_max_retry")
        ctk.CTkLabel(frame, text="apsta_policy").grid(row=2, column=0, sticky="w", padx=8, pady=4)
        apsta_select = ctk.CTkOptionMenu(
            frame,
            variable=self.settings_vars["apsta_policy_label"],
            values=list(APSTA_POLICY_LABEL_TO_VALUE.keys()),
        )
        apsta_select.grid(row=2, column=1, sticky="ew", padx=8, pady=4)
        self.settings_inputs["apsta_policy"] = apsta_select
        self._settings_entry(frame, 2, 2, "apsta_grace_period_s", self.settings_vars["apsta_grace_period_s"], key_name="apsta_grace_period_s")
        return row_start + 1

    def _build_settings_section_trigger_output(self, parent, row_start: int) -> int:
        frame = ctk.CTkFrame(parent)
        frame.grid(row=row_start, column=0, sticky="ew", pady=(0, 8))
        frame.grid_columnconfigure(1, weight=1)
        frame.grid_columnconfigure(3, weight=1)
        ctk.CTkLabel(frame, text="Saida BT_NEXT", font=ctk.CTkFont(size=14, weight="bold")).grid(
            row=0, column=0, columnspan=4, sticky="w", padx=8, pady=(8, 6)
        )
        ctk.CTkCheckBox(frame, text="trigger_output.enabled", variable=self.settings_vars["trigger_enabled"]).grid(
            row=1, column=0, sticky="w", padx=8, pady=4
        )
        self._settings_entry(frame, 1, 2, "trigger_output.gpio", self.settings_vars["trigger_gpio"], key_name="trigger_gpio")
        self._settings_entry(frame, 2, 0, "trigger_output.active_level", self.settings_vars["trigger_active_level"], key_name="trigger_active_level")
        self._settings_entry(frame, 2, 2, "trigger_output.pulse_ms", self.settings_vars["trigger_pulse_ms"], key_name="trigger_pulse_ms")
        return row_start + 1

    def _build_settings_section_acr_cloud(self, parent, row_start: int):
        frame = ctk.CTkFrame(parent)
        frame.grid(row=row_start, column=0, sticky="ew", pady=(0, 8))
        frame.grid_columnconfigure(1, weight=1)
        frame.grid_columnconfigure(3, weight=1)
        ctk.CTkLabel(frame, text="ACRCloud", font=ctk.CTkFont(size=14, weight="bold")).grid(
            row=0, column=0, columnspan=4, sticky="w", padx=8, pady=(8, 6)
        )
        self._settings_entry(frame, 1, 0, "region", self.settings_vars["acr_region"], key_name="acr_region")
        self._settings_entry(frame, 1, 2, "container_id", self.settings_vars["acr_container_id"], key_name="acr_container_id")
        self._settings_entry(frame, 2, 0, "upload_prefix", self.settings_vars["acr_upload_prefix"], key_name="acr_upload_prefix")
        self._settings_entry(frame, 2, 2, "bearer_token", self.settings_vars["acr_bearer_token"], show="*", key_name="acr_bearer_token")

    def _settings_entry(self, parent, row: int, col: int, label: str, variable, show=None, key_name: Optional[str] = None):
        ctk.CTkLabel(parent, text=label).grid(row=row, column=col, sticky="w", padx=8, pady=4)
        entry = ctk.CTkEntry(parent, textvariable=variable, show=show)
        entry.grid(row=row, column=col + 1, sticky="ew", padx=8, pady=4)
        if key_name:
            self.settings_inputs[key_name] = entry

    def _build_status_panel(self, parent):
        parent.grid_columnconfigure(0, weight=1)
        parent.grid_rowconfigure(0, weight=0)
        parent.grid_rowconfigure(1, weight=1)

        top = ctk.CTkFrame(parent)
        top.grid(row=0, column=0, sticky="ew", padx=8, pady=(4, 2))
        top.grid_columnconfigure(0, weight=0)
        top.grid_columnconfigure(1, weight=0)
        top.grid_columnconfigure(2, weight=1)
        top.grid_columnconfigure(3, weight=0)
        top.grid_columnconfigure(4, weight=0)
        top.grid_columnconfigure(5, weight=0)
        top.grid_columnconfigure(6, weight=0)

        ctk.CTkLabel(top, text="Status tecnico", font=ctk.CTkFont(size=16, weight="bold")).grid(
            row=0, column=0, sticky="w", padx=(8, 12), pady=(4, 0)
        )
        self.status_time_label = ctk.CTkLabel(
            top,
            text="Atualizacao: --",
            anchor="w",
            justify="left",
            text_color=("gray40", "gray70"),
        )
        self.status_time_label.grid(row=1, column=0, columnspan=7, sticky="ew", padx=8, pady=(0, 4))
        self._bind_delayed_tooltip(self.status_time_label, lambda: self.status_time_label.cget("text"))
        toolbar_spacer = ctk.CTkFrame(top, fg_color="transparent", height=1)
        toolbar_spacer.grid(row=0, column=2, sticky="ew")
        ctk.CTkCheckBox(top, text="Auto", variable=self.technical_auto_update_var, width=72).grid(
            row=0, column=1, sticky="e", padx=(4, 4), pady=(4, 0)
        )
        ctk.CTkLabel(top, text="s").grid(row=0, column=3, sticky="e", padx=(4, 2), pady=(4, 0))
        ctk.CTkEntry(top, textvariable=self.technical_update_interval_var, width=64).grid(
            row=0, column=4, sticky="e", padx=(2, 4), pady=(4, 0)
        )
        ctk.CTkButton(top, text="get_state", width=120, command=self._request_get_state_selected).grid(
            row=0, column=5, sticky="e", padx=(4, 4), pady=(4, 0)
        )
        ctk.CTkButton(top, text="status_tecnico", width=140, command=self._request_technical_status_selected).grid(
            row=0, column=6, sticky="e", padx=(4, 8), pady=(4, 0)
        )

        self.status_body = ctk.CTkScrollableFrame(parent)
        self.status_body.grid(row=1, column=0, sticky="nsew", padx=8, pady=(0, 6))
        self.status_body.grid_columnconfigure((0, 1, 2, 3), weight=1, uniform="status_cards")
        self.mousewheel_scroll_frames.append(self.status_body)

        self.status_empty_label = ctk.CTkLabel(
            self.status_body,
            text="Selecione um dispositivo para visualizar status.",
            anchor="w",
        )

        card_specs = [
            ("loop", "Loop"),
            ("automatic", "Automatico"),
            ("last_result", "Ultimo resultado"),
            ("audio", "Audio"),
            ("network", "Rede"),
            ("errors_retry", "Erros / retry"),
            ("counters", "Contadores"),
            ("vbat", "VBAT"),
        ]
        for index, (key, title) in enumerate(card_specs):
            self._status_card(self.status_body, index // 4, index % 4, key, title)

        row = 2
        ctk.CTkLabel(self.status_body, text="Tempos", font=ctk.CTkFont(size=14, weight="bold")).grid(
            row=row, column=0, columnspan=4, sticky="w", padx=8, pady=(12, 4)
        )
        row += 1
        timing_frame = ctk.CTkFrame(self.status_body)
        timing_frame.grid(row=row, column=0, columnspan=4, sticky="ew", padx=8, pady=(0, 8))
        timings = [
            ("capture", "Captura"),
            ("upload", "Upload"),
            ("upload_connect", "TLS"),
            ("upload_write", "Envio"),
            ("upload_response", "POST"),
            ("response_wait", "ACR"),
            ("total_cycle", "Total"),
        ]
        for col, (key, label) in enumerate(timings):
            timing_frame.grid_columnconfigure(col, weight=1)
            ctk.CTkLabel(timing_frame, text=label, text_color=("gray40", "gray70")).grid(
                row=0, column=col, sticky="w", padx=8, pady=(8, 0)
            )
            value_label = ctk.CTkLabel(timing_frame, text="--", font=ctk.CTkFont(weight="bold"))
            value_label.grid(row=1, column=col, sticky="w", padx=8, pady=(0, 8))
            self.status_value_labels[f"timing.{key}"] = value_label

        row += 1
        ctk.CTkLabel(self.status_body, text="Campos brutos", font=ctk.CTkFont(size=14, weight="bold")).grid(
            row=row, column=0, columnspan=4, sticky="w", padx=8, pady=(12, 4)
        )
        row += 1
        self.status_raw_text = ctk.CTkTextbox(self.status_body, height=260, wrap="word")
        self.status_raw_text.grid(row=row, column=0, columnspan=4, sticky="ew", padx=8, pady=(0, 8))
        self.status_raw_text.insert(END, "-")
        self.status_raw_text.configure(state="disabled")

    def _enable_mousewheel_scrolling(self):
        self.bind_all("<MouseWheel>", self._on_global_mousewheel, add="+")
        self.bind_all("<Button-4>", self._on_global_mousewheel, add="+")
        self.bind_all("<Button-5>", self._on_global_mousewheel, add="+")

    def _on_global_mousewheel(self, event):
        widget = self.winfo_containing(event.x_root, event.y_root)
        if widget is None:
            return

        target_canvas = self._resolve_scroll_canvas_for_widget(widget)
        if target_canvas is None:
            return

        if getattr(event, "num", None) == 4:
            step = -1
        elif getattr(event, "num", None) == 5:
            step = 1
        else:
            delta = getattr(event, "delta", 0)
            if delta == 0:
                return
            step = -1 if delta > 0 else 1

        target_canvas.yview_scroll(step, "units")

    def _resolve_scroll_canvas_for_widget(self, widget):
        for frame in self.mousewheel_scroll_frames:
            if not frame.winfo_exists():
                continue
            if self._is_widget_descendant_of(widget, frame):
                parent_canvas = getattr(frame, "_parent_canvas", None)
                if parent_canvas is not None:
                    return parent_canvas
        return None

    def _is_widget_descendant_of(self, widget, ancestor) -> bool:
        current = widget
        while current is not None:
            if str(current) == str(ancestor):
                return True
            parent_name = current.winfo_parent()
            if not parent_name:
                return False
            try:
                current = current.nametowidget(parent_name)
            except Exception:
                return False
        return False

    def _flatten_dict(self, payload: Dict[str, Any], prefix: str = "") -> Dict[str, Any]:
        out: Dict[str, Any] = {}
        for key, value in payload.items():
            path = f"{prefix}.{key}" if prefix else key
            if isinstance(value, dict):
                out.update(self._flatten_dict(value, path))
            else:
                out[path] = value
        return out

    def _refresh_status_panel(self):
        if not hasattr(self, "status_body"):
            return

        if not self.selected_device:
            self._show_status_empty("Selecione um dispositivo para visualizar status.")
            self._clear_status_values()
            return

        device = self.devices.get(self.selected_device)
        if not device:
            self._show_status_empty("Dispositivo sem dados.")
            self._clear_status_values()
            return

        state_topic_payload = self._payload_for(device, "state")
        get_state_payload = device.last_get_state_result or {}
        technical_status_payload = device.last_technical_status_result or {}
        heartbeat_payload = self._payload_for(device, "heartbeat")
        control_payload = technical_status_payload.get("acr_control", {})
        vbat_payload = technical_status_payload.get("vbat", {})
        power_good_payload = technical_status_payload.get("power_good", {})

        merged: Dict[str, Any] = {}
        if state_topic_payload:
            merged.update(self._flatten_dict(state_topic_payload))
        if get_state_payload:
            merged.update(self._flatten_dict(get_state_payload))
        if technical_status_payload:
            merged.update(self._flatten_dict(technical_status_payload))

        if not merged:
            self._show_status_empty("Sem campos de status ainda. Clique em get_state ou status_tecnico.")
            self._clear_status_values()
        else:
            self._hide_status_empty()
            self._render_technical_status_view(
                technical_status_payload,
                get_state_payload,
                state_topic_payload,
                heartbeat_payload,
                control_payload if isinstance(control_payload, dict) else {},
                vbat_payload if isinstance(vbat_payload, dict) else {},
                power_good_payload if isinstance(power_good_payload, dict) else {},
                merged,
            )

        source = []
        if state_topic_payload:
            source.append("topic state")
        if get_state_payload:
            source.append("cmd get_state")
        if technical_status_payload:
            source.append("cmd status_tecnico")
        source_text = ", ".join(source) if source else "sem fonte"
        last_tech = self._format_local_datetime(device.last_technical_status_at)
        if hasattr(self, "status_time_label"):
            self.status_time_label.configure(
                text=(
                    f"Ultima renderizacao: {datetime.now().strftime('%Y-%m-%d %H:%M:%S')} | "
                    f"ultimo status tecnico: {last_tech} | fonte: {source_text}"
                )
            )

    def _show_status_empty(self, text: str):
        if hasattr(self, "status_empty_label"):
            self.status_empty_label.configure(text=text)
            self.status_empty_label.grid_remove()
        if hasattr(self, "status_time_label"):
            self.status_time_label.configure(text=text)

    def _hide_status_empty(self):
        if hasattr(self, "status_empty_label"):
            self.status_empty_label.grid_remove()

    def _clear_status_values(self):
        for key in self.status_value_labels:
            self._set_status_label(key, "--")
        if hasattr(self, "status_raw_text"):
            self._set_status_raw_text("-")

    def _set_status_label(self, key: str, value: Any):
        label = self.status_value_labels.get(key)
        if label:
            text = str(value) if value not in (None, "") else "--"
            if self.status_display_values.get(key) != text:
                self.status_display_values[key] = text
                label.configure(text=text)

    def _set_status_raw_text(self, text: str):
        if not hasattr(self, "status_raw_text"):
            return
        if self.status_raw_text_value == text:
            return
        self.status_raw_text_value = text
        self.status_raw_text.configure(state="normal")
        self.status_raw_text.delete("1.0", END)
        self.status_raw_text.insert(END, text)
        self.status_raw_text.configure(state="disabled")

    def _render_technical_status_view(
        self,
        tech: Dict[str, Any],
        get_state: Dict[str, Any],
        state_topic: Dict[str, Any],
        heartbeat: Dict[str, Any],
        control: Dict[str, Any],
        vbat: Dict[str, Any],
        power_good: Dict[str, Any],
        merged: Dict[str, Any],
    ):
        retry_text = (
            f"retry em {self._format_ms(self._num(tech, 'acr_retry_remaining_ms'))}"
            if bool(tech.get("acr_retry_pending"))
            else "sem retry"
        )
        last_result = "--"
        if bool(tech.get("audio_last_silence_discarded")):
            last_result = "Silencio descartado"
        elif self._num(tech, "acr_last_result_at_ms", 0) > 0:
            actor = "IA / trigger" if bool(tech.get("acr_last_trigger")) else "Humano"
            last_result = f"{actor} | {self._num(tech, 'acr_last_ai_probability', 0):.1f}%"

        auto_text = "--"
        if control:
            auto_text = (
                f"{'Ligado' if bool(control.get('automatic_enabled')) else 'Desligado'} | "
                f"int {self._format_ms(self._num(control, 'automatic_interval_ms'))} | "
                f"capt {self._value(control, 'capture_duration_seconds', '--')}s | "
                f"ganho {self._num(control, 'digital_gain', 0):.2f}x"
            )

        audio_detail = (
            f"RMS {self._num(tech, 'audio_last_rms', 0):.0f} | "
            f"pico {self._num(tech, 'audio_last_peak_percent', 0):.1f}% | "
            f"silencio {'sim' if bool(tech.get('audio_last_silence_detected')) else 'nao'} | "
            f"clipped {'sim' if bool(tech.get('audio_last_clipped_detected')) else 'nao'}"
        )
        counters_text = (
            f"envios {self._value(tech, 'acr_submitted_count', 0)} | "
            f"silencio {self._value(tech, 'acr_silence_discarded_count', 0)} | "
            f"erros ACR {self._value(tech, 'acr_error_count', 0)}"
        )
        vbat_text = self._format_vbat(vbat, get_state)
        power_good_text = self._format_power_good(power_good, vbat)
        network_payloads = [get_state, state_topic, heartbeat]
        network_ssid = "-"
        network_ip = "-"
        network_rssi = "-"
        for payload in network_payloads:
            if not isinstance(payload, dict):
                continue
            if network_ssid == "-":
                network_ssid = self._field(payload, "ssid", "wifi_ssid")
            if network_ip == "-":
                network_ip = self._field(payload, "ip", "ipv4", "sta_ip", "wifi_ip")
            if network_rssi == "-":
                network_rssi = self._field(payload, "rssi")
        network_value = f"IP {network_ip}"
        if network_rssi != "-":
            network_value += f" | RSSI {network_rssi}"
        uptime_value = (
            self._num(tech, "uptime_seconds", None)
            if tech
            else self._num(get_state, "uptime_s", self._num(heartbeat, "uptime_s", None))
        )

        self._set_status_label("card.loop.value", self._format_state(str(self._value(tech, "acr_state", self._value(state_topic, "wifi_state", "--")))))
        self._set_status_label("card.loop.detail", self._value(tech, "acr_status", ""))
        self._set_status_label("card.automatic.value", auto_text)
        self._set_status_label(
            "card.automatic.detail",
            f"hyst {self._value(control, 'silence_hysteresis_rms', '--')} | min ativo {self._format_ms(self._num(control, 'min_active_ms'))}",
        )
        self._set_status_label("card.last_result.value", last_result)
        self._set_status_label("card.last_result.detail", self._value(tech, "acr_last_prediction", "--"))
        self._set_status_label("card.audio.value", self._format_ms(self._num(tech, "audio_last_active_ms")))
        self._set_status_label("card.audio.detail", audio_detail)
        self._set_status_label("card.network.value", network_value)
        self._set_status_label("card.network.detail", f"SSID {network_ssid}")
        self._set_status_label("card.errors_retry.value", f"{self._value(tech, 'acr_consecutive_errors', 0)} erros | {retry_text}")
        self._set_status_label("card.errors_retry.detail", self._value(tech, "acr_last_error", ""))
        self._set_status_label("card.counters.value", counters_text)
        self._set_status_label("card.counters.detail", f"uptime {self._format_uptime(uptime_value)}")
        self._set_status_label("card.vbat.value", vbat_text)
        self._set_status_label("card.vbat.detail", power_good_text)

        timing_keys = {
            "capture": "acr_capture_ms",
            "upload": "acr_upload_ms",
            "upload_connect": "acr_upload_connect_ms",
            "upload_write": "acr_upload_write_ms",
            "upload_response": "acr_upload_response_ms",
            "response_wait": "acr_response_wait_ms",
            "total_cycle": "acr_total_cycle_ms",
        }
        for label_key, payload_key in timing_keys.items():
            self._set_status_label(f"timing.{label_key}", self._format_ms(self._num(tech, payload_key)))

        raw_lines = []
        for key in sorted(merged.keys()):
            value = merged[key]
            value_text = json.dumps(value, ensure_ascii=True) if isinstance(value, (dict, list)) else str(value)
            raw_lines.append(f"{key}: {value_text}")
        self._set_status_raw_text("\n".join(raw_lines) if raw_lines else "-")

    def _status_card(self, parent, row: int, col: int, key: str, title: str):
        card = ctk.CTkFrame(parent)
        card.grid(row=row, column=col, sticky="nsew", padx=6, pady=6)
        card.grid_columnconfigure(0, weight=1)
        ctk.CTkLabel(card, text=title, text_color=("gray40", "gray70")).grid(
            row=0, column=0, sticky="w", padx=10, pady=(10, 2)
        )
        value_label = ctk.CTkLabel(
            card,
            text="--",
            anchor="w",
            justify="left",
            font=ctk.CTkFont(size=14, weight="bold"),
            wraplength=180,
        )
        value_label.grid(
            row=1, column=0, sticky="ew", padx=10, pady=(0, 2)
        )
        detail_label = ctk.CTkLabel(
            card,
            text=" ",
            anchor="w",
            justify="left",
            text_color=("gray45", "gray65"),
            wraplength=180,
        )
        detail_label.grid(
            row=2, column=0, sticky="ew", padx=10, pady=(0, 10)
        )
        card.bind(
            "<Configure>",
            lambda event, labels=(value_label, detail_label): [
                label.configure(wraplength=max(120, event.width - 28)) for label in labels
            ],
            add="+",
        )
        self.status_value_labels[f"card.{key}.value"] = value_label
        self.status_value_labels[f"card.{key}.detail"] = detail_label

    def _value(self, payload: Dict[str, Any], key: str, default: Any = "--") -> Any:
        if not isinstance(payload, dict):
            return default
        value = payload.get(key)
        return default if value is None or value == "" else value

    def _num(self, payload: Dict[str, Any], key: str, default: Any = 0) -> Any:
        if not isinstance(payload, dict):
            return default
        value = payload.get(key)
        if value is None or value == "":
            return default
        try:
            return float(value)
        except (TypeError, ValueError):
            return default

    def _format_ms(self, value: Any) -> str:
        try:
            numeric = float(value)
        except (TypeError, ValueError):
            return "--"
        if numeric <= 0:
            return "--"
        if numeric < 1000:
            return f"{numeric:.0f} ms"
        return f"{numeric / 1000:.2f} s"

    def _format_state(self, value: str) -> str:
        states = {
            "idle": "Idle",
            "capturing": "Capturando",
            "silence_discarded": "Silencio descartado",
            "uploading": "Enviando",
            "waiting_acr": "Aguardando ACR",
            "result_human": "Humano",
            "result_ai": "IA",
            "error": "Erro",
            "retry_wait": "Retry",
            "sta_connected": "Wi-Fi conectado",
            "sta_connecting": "Wi-Fi conectando",
            "provisioning_ap": "Provisionamento",
            "init": "Inicializando",
        }
        return states.get(value, value or "--")

    def _format_uptime(self, seconds_value: Any) -> str:
        try:
            total = int(float(seconds_value))
        except (TypeError, ValueError):
            return "--"
        if total < 0:
            return "--"
        days = total // 86400
        hours = (total % 86400) // 3600
        minutes = (total % 3600) // 60
        seconds = total % 60
        text = f"{hours:02d}:{minutes:02d}:{seconds:02d}"
        return f"{days}d {text}" if days else text

    def _format_vbat(self, vbat: Dict[str, Any], get_state: Dict[str, Any]) -> str:
        if isinstance(vbat, dict) and vbat:
            if not bool(vbat.get("enabled")):
                return "Desabilitado"
            if not bool(vbat.get("initialized")):
                return "Nao inicializado"
            if not self._num(vbat, "measurement_count", 0):
                return f"Sem amostra | GPIO {self._value(vbat, 'gpio', '--')}"
            voltage = self._num(vbat, "vbat_mv", 0) / 1000
            return (
                f"{voltage:.3f} V | GPIO {self._value(vbat, 'gpio', '--')} | "
                f"{self._value(vbat, 'last_moment', '-')}"
            )
        vbat_mv = self._num(get_state, "vbat_mv", None)
        if vbat_mv is None:
            return "--"
        return f"{vbat_mv / 1000:.3f} V"

    def _format_power_good(self, power_good: Dict[str, Any], vbat: Dict[str, Any]) -> str:
        pg_enabled = bool(power_good.get("enabled")) if isinstance(power_good, dict) else False
        pg_gpio = self._value(power_good, "gpio", "--") if isinstance(power_good, dict) else "--"
        shutdown_enabled = bool(vbat.get("shutdown_enabled")) if isinstance(vbat, dict) else False
        threshold_mv = self._value(vbat, "shutdown_threshold_mv", "--") if isinstance(vbat, dict) else "--"
        countdown_active = bool(vbat.get("shutdown_countdown_active")) if isinstance(vbat, dict) else False
        countdown_elapsed = self._format_ms(self._num(vbat, "shutdown_countdown_elapsed_ms")) if isinstance(vbat, dict) else "--"
        if shutdown_enabled:
            countdown_text = f"ativo ({countdown_elapsed})" if countdown_active else "inativo"
        else:
            countdown_text = "desabilitado"
        return (
            f"PG {'on' if pg_enabled else 'off'} | GPIO {pg_gpio} | "
            f"threshold {threshold_mv} mV | countdown {countdown_text}"
        )

    def _build_details_panel(self):
        ctk.CTkLabel(self.details, text="Resumo do Dispositivo", font=ctk.CTkFont(size=16, weight="bold")).grid(
            row=0, column=0, columnspan=2, sticky="w", padx=8, pady=(8, 10)
        )

        rows = [
            ("Device ID", "device_id"),
            ("Online", "online"),
            ("Last seen (local)", "last_seen"),
            ("Wi-Fi SSID", "wifi_ssid"),
            ("IP", "ip"),
            ("FW", "fw"),
            ("Session", "session_id"),
            ("Heartbeat ts", "hb_ts"),
            ("Heartbeat uptime_s", "hb_uptime"),
            ("Heartbeat rssi", "hb_rssi"),
            ("Heartbeat heap_free", "hb_heap"),
            ("Heartbeat vbat", "hb_vbat"),
            ("State", "state"),
            ("Status", "status"),
            ("Event", "event"),
            ("Event message", "event_message"),
            ("Cmd/out ts", "cmd_ts"),
            ("Cmd ID", "cmd_id"),
            ("Cmd OK", "cmd_ok"),
            ("Cmd error", "cmd_error"),
            ("Cmd result", "cmd_result"),
            ("State payload", "state_payload"),
            ("Status payload", "status_payload"),
            ("Heartbeat payload", "heartbeat_payload"),
        ]

        for index, (label, key) in enumerate(rows, start=1):
            ctk.CTkLabel(self.details, text=label).grid(row=index, column=0, sticky="nw", padx=(8, 8), pady=2)
            value = ctk.CTkLabel(self.details, text="-", anchor="w", justify="left", wraplength=700)
            value.grid(row=index, column=1, sticky="ew", padx=(0, 8), pady=2)
            self.detail_value_labels[key] = value

    def _labeled_entry(self, parent, row, label, variable, show=None):
        ctk.CTkLabel(parent, text=label).grid(row=row, column=0, sticky="w", padx=8, pady=4)
        ctk.CTkEntry(parent, textvariable=variable, show=show).grid(row=row, column=1, sticky="ew", padx=8, pady=4)

    def _load_example_or_local_config(self):
        config_data = None
        for filename in ("config.json", "config.example.json"):
            path = Path(filename)
            if path.exists():
                try:
                    config_data = json.loads(path.read_text(encoding="utf-8"))
                    self._append_log(f"Loaded configuration from {filename}", tag="info")
                    break
                except Exception as exc:
                    self._append_log(f"Could not load {filename}: {exc}", tag="warn")

        if not isinstance(config_data, dict):
            return

        mqtt_cfg = config_data.get("mqtt", {})
        broker_url = mqtt_cfg.get("url")
        if broker_url:
            self.host_var.set(str(broker_url))
        else:
            self.host_var.set(str(mqtt_cfg.get("host", self.host_var.get())))
        self.port_var.set(str(mqtt_cfg.get("port", self.port_var.get())))
        self.user_var.set(str(mqtt_cfg.get("username", self.user_var.get())))
        self.pass_var.set(str(mqtt_cfg.get("password", self.pass_var.get())))
        self.tls_var.set(bool(mqtt_cfg.get("tls", self.tls_var.get())))
        self.heartbeat_timeout_var.set(str(config_data.get("heartbeat_timeout_sec", self.heartbeat_timeout_var.get())))
        self.technical_update_interval_var.set(
            str(config_data.get("technical_status_update_interval_sec", self.technical_update_interval_var.get()))
        )
        self.technical_auto_update_var.set(
            bool(config_data.get("technical_status_auto_update", self.technical_auto_update_var.get()))
        )
        self.auto_connect_on_start_var.set(
            bool(config_data.get("auto_connect_on_start", self.auto_connect_on_start_var.get()))
        )

    def _auto_connect_if_configured(self):
        if self.auto_connect_pending and bool(self.auto_connect_on_start_var.get()) and not self.mqtt.connected:
            self._append_log("Auto connect on start habilitado", tag="info")
            self.auto_connect_pending = False
            self._connect()

    def _set_connection_ui_state(self, state: str):
        if not hasattr(self, "btn_connect") or not hasattr(self, "btn_disconnect"):
            return
        if state == "starting":
            self.btn_connect.configure(state="disabled", text="Connect")
            self.btn_disconnect.configure(state="disabled", text="Disconnect")
        elif state == "auto_connect":
            self.btn_connect.configure(state="disabled", text="Connect")
            self.btn_disconnect.configure(state="normal", text="Cancel")
        elif state == "connecting":
            self.btn_connect.configure(state="disabled", text="Connecting...")
            self.btn_disconnect.configure(state="normal", text="Cancel")
        elif state == "connected":
            self.btn_connect.configure(state="disabled", text="Connect")
            self.btn_disconnect.configure(state="normal", text="Disconnect")
        else:
            self.btn_connect.configure(state="normal", text="Connect")
            self.btn_disconnect.configure(state="disabled", text="Disconnect")

    def _set_disconnected_visual_state(self, reason: str = "Disconnected"):
        self.auto_connect_pending = False
        self.conn_state_var.set(reason)
        self.conn_indicator.configure(fg_color="#a33")
        self._set_connection_ui_state("disconnected")

    def _set_auto_connect_visual_state(self):
        self.auto_connect_pending = True
        self.conn_state_var.set("Auto-connect...")
        self.conn_indicator.configure(fg_color="#0b5ed7")
        self._set_connection_ui_state("auto_connect")

    def _set_connecting_visual_state(self):
        self.auto_connect_pending = False
        self.conn_state_var.set("Connecting...")
        self.conn_indicator.configure(fg_color="#d67a00")
        self._set_connection_ui_state("connecting")

    def _connect(self):
        if self.mqtt.connected:
            return

        host, port, tls_enabled, error = self._resolve_broker_params(
            self.host_var.get().strip(),
            self.port_var.get().strip(),
            bool(self.tls_var.get()),
        )
        if error:
            self._append_log(error, tag="error")
            self._set_disconnected_visual_state("Disconnected")
            return

        self._set_connecting_visual_state()
        self.mqtt.connect(
            host=host,
            port=port,
            username=self.user_var.get().strip(),
            password=self.pass_var.get(),
            tls_enabled=tls_enabled,
        )

    def _resolve_broker_params(
        self,
        host_input: str,
        port_input: str,
        tls_input: bool,
    ) -> Tuple[str, int, bool, Optional[str]]:
        if not host_input:
            return "", 0, tls_input, "Host is required"

        host = host_input
        tls_enabled = tls_input
        port = 0

        if host_input.startswith(("mqtt://", "mqtts://")):
            parsed = urlparse(host_input)
            if not parsed.hostname:
                return "", 0, tls_input, "Invalid MQTT URL"
            host = parsed.hostname
            if parsed.scheme == "mqtts":
                tls_enabled = True
                port = parsed.port or 8883
            else:
                tls_enabled = False
                port = parsed.port or 1883

        if port == 0:
            try:
                port = int(port_input)
            except ValueError:
                return "", 0, tls_enabled, "Port must be an integer"

        return host, port, tls_enabled, None

    def _disconnect(self):
        if self.auto_connect_pending and not self.mqtt.connected:
            self._append_log("Auto connect cancelado", tag="warn")
            self._set_disconnected_visual_state("Disconnected")
            return
        self._set_disconnected_visual_state("Disconnected")
        self.mqtt.disconnect()

    def _on_select_device(self, _event=None):
        selection = self.tree.selection()
        if not selection:
            self.selected_device = None
            self._refresh_status_panel()
            return

        self.selected_device = selection[0]
        self.clear_retained_device_var.set(self.selected_device)
        self._update_settings_selection_status()
        self._refresh_device_details()
        self._refresh_status_panel()

    def _update_settings_selection_status(self):
        if not hasattr(self, "settings_status_label") or not self.settings_loaded:
            return
        if self.settings_loaded_device_id and self.selected_device != self.settings_loaded_device_id:
            previous_device = self.settings_loaded_device_id
            self._clear_settings_form_state(
                f"Settings de {previous_device} limpos ao trocar device. Leia settings do device atual."
            )

    def _on_device_tree_motion(self, event):
        row_id = self.tree.identify_row(event.y)
        col_id = self.tree.identify_column(event.x)
        if not row_id or not col_id:
            self._hide_tooltip()
            return

        tooltip_key = (row_id, col_id)
        if getattr(self, "tooltip_tree_cell", None) == tooltip_key and self.tooltip_after_id is None:
            self._remember_tooltip_motion(event)
            return

        text = self._device_tree_cell_tooltip_text(row_id, col_id)
        if not text:
            self._hide_tooltip()
            return

        if getattr(self, "tooltip_tree_cell", None) != tooltip_key:
            self._hide_tooltip()
            self.tooltip_tree_cell = tooltip_key
            self._schedule_tooltip(self.tree, lambda value=text: value, event, 650)
        else:
            self._remember_tooltip_motion(event)

    def _device_tree_cell_tooltip_text(self, row_id: str, col_id: str) -> str:
        device = self.devices.get(row_id)
        if not device:
            return ""
        col_index = int(col_id.lstrip("#")) - 1
        columns = list(self.tree["columns"])
        if col_index < 0 or col_index >= len(columns):
            return ""
        column_name = columns[col_index]
        if column_name == "summary":
            ssid, ip, rssi = self._device_network_info(device)
            return f"{self._device_summary(device)} | ssid: {ssid} | ip: {ip} | rssi: {rssi}"
        if column_name == "age":
            return f"idade {self._format_age(device.last_seen)} | ultimo contato: {self._format_local_datetime(device.last_seen)}"
        if column_name == "presence":
            _presence_key, presence_text = self._device_presence(device)
            return presence_text
        if column_name == "device_id":
            return device.device_id
        if column_name == "fw":
            return device.fw
        return ""

    def _show_device_context_menu(self, event):
        row_id = self.tree.identify_row(event.y)
        if not row_id:
            self._hide_device_context_menu()
            return "break"

        self.tree.selection_set(row_id)
        self.selected_device = row_id
        self.clear_retained_device_var.set(row_id)
        self._refresh_device_details()
        self._refresh_status_panel()
        self._hide_device_context_menu()

        menu = ctk.CTkToplevel(self)
        menu.withdraw()
        menu.overrideredirect(True)
        menu.attributes("-topmost", True)
        menu.configure(fg_color=("gray96", "gray15"))

        frame = ctk.CTkFrame(
            menu,
            corner_radius=8,
            border_width=1,
            border_color=("gray70", "gray35"),
            fg_color=("gray96", "gray15"),
        )
        frame.grid(row=0, column=0, sticky="nsew")

        device = self.devices.get(row_id)
        action_enabled = bool(device and device.online and self.mqtt.connected)
        if device:
            _presence_key, presence_text = self._device_presence(device)
            ssid, ip, _rssi = self._device_network_info(device)
            header_lines = [
                row_id,
                f"{presence_text} | idade {self._format_age(device.last_seen)}",
                f"ultimo contato: {self._format_local_datetime(device.last_seen)}",
                f"ssid: {ssid} | ip: {ip}",
                self._device_summary(device),
            ]
        else:
            header_lines = [row_id, "sem dados", "ultimo contato: -", "ssid: - | ip: -", "-"]

        header = ctk.CTkFrame(frame, fg_color=("gray90", "gray22"), corner_radius=6)
        header.grid(row=0, column=0, sticky="ew", padx=5, pady=5)
        ctk.CTkLabel(
            header,
            text=header_lines[0],
            anchor="w",
            justify="left",
            font=ctk.CTkFont(size=13, weight="bold"),
        ).grid(row=0, column=0, sticky="ew", padx=8, pady=(6, 0))
        ctk.CTkLabel(
            header,
            text="\n".join(header_lines[1:]),
            anchor="w",
            justify="left",
            text_color=("gray35", "gray75"),
            wraplength=260,
        ).grid(row=1, column=0, sticky="ew", padx=8, pady=(0, 6))

        actions = [
            ("Copiar device_id", lambda device_id=row_id: self._copy_device_id(device_id), True),
            ("Copiar IP", lambda device_id=row_id: self._copy_device_ip(device_id), True),
            ("get_state", lambda device_id=row_id: self._send_context_command(device_id, "get_state", "Status"), action_enabled),
            (
                "status_tecnico",
                lambda device_id=row_id: self._send_context_command(device_id, "get_technical_status", "Status"),
                action_enabled,
            ),
            ("get_settings", lambda device_id=row_id: self._request_get_settings_for_device(device_id), action_enabled),
            ("Limpar retained", lambda device_id=row_id: self._clear_retained_for_device_id(device_id), True),
            ("Abrir Status", lambda device_id=row_id: self._select_device_tab(device_id, "Status"), True),
            ("Abrir Settings", lambda device_id=row_id: self._select_device_tab(device_id, "Settings"), True),
        ]
        for index, (text, command, enabled) in enumerate(actions, start=1):
            ctk.CTkButton(
                frame,
                text=text,
                width=280,
                height=30,
                corner_radius=6,
                fg_color="transparent",
                hover_color=("gray84", "gray28"),
                text_color=("gray10", "gray90") if enabled else ("gray55", "gray45"),
                anchor="w",
                state="normal" if enabled else "disabled",
                command=lambda cmd=command: self._run_device_context_action(cmd),
            ).grid(
                row=index,
                column=0,
                sticky="ew",
                padx=5,
                pady=(1, 5 if index == len(actions) else 1),
        )

        menu.update_idletasks()
        x_pos, y_pos = self._context_menu_position(
            event.x_root,
            event.y_root,
            menu.winfo_reqwidth(),
            menu.winfo_reqheight(),
        )
        menu.geometry(f"+{x_pos}+{y_pos}")
        menu.deiconify()
        menu.lift()
        menu.focus_force()
        menu.bind("<FocusOut>", self._hide_device_context_menu, add="+")
        self.device_context_menu = menu
        return "break"

    def _run_device_context_action(self, command):
        self._hide_device_context_menu()
        command()

    def _hide_device_context_menu(self, _event=None):
        menu = getattr(self, "device_context_menu", None)
        if menu is not None:
            try:
                menu.destroy()
            except tk.TclError:
                pass
            self.device_context_menu = None

    def _hide_device_context_menu_if_outside(self, event):
        menu = getattr(self, "device_context_menu", None)
        if menu is None:
            return
        try:
            x0 = menu.winfo_rootx()
            y0 = menu.winfo_rooty()
            x1 = x0 + menu.winfo_width()
            y1 = y0 + menu.winfo_height()
        except tk.TclError:
            self.device_context_menu = None
            return

        if not (x0 <= event.x_root <= x1 and y0 <= event.y_root <= y1):
            self._hide_device_context_menu()

    def _context_menu_position(self, x_root: int, y_root: int, width: int, height: int) -> Tuple[int, int]:
        margin = 8
        root_x = self.winfo_rootx()
        root_y = self.winfo_rooty()
        root_width = self.winfo_width()
        root_height = self.winfo_height()

        min_x = root_x + margin
        max_x = max(min_x, root_x + root_width - width - margin)
        min_y = root_y + margin
        max_y = max(min_y, root_y + root_height - height - margin)

        x_pos = min(max(min_x, x_root), max_x)
        y_pos = min(max(min_y, y_root - height - margin), max_y)
        return x_pos, y_pos

    def _bind_delayed_tooltip(self, widget, text_provider, delay_ms: int = 700):
        widget.bind("<Enter>", lambda event, w=widget, provider=text_provider: self._schedule_tooltip(w, provider, event, delay_ms), add="+")
        widget.bind("<Motion>", self._remember_tooltip_motion, add="+")
        widget.bind("<Leave>", self._hide_tooltip, add="+")

    def _schedule_tooltip(self, widget, text_provider, event, delay_ms: int):
        self._cancel_tooltip_schedule()
        self.tooltip_widget = widget
        self.tooltip_last_xy = (event.x_root, event.y_root)
        self.tooltip_after_id = self.after(delay_ms, lambda: self._show_tooltip(widget, text_provider))

    def _remember_tooltip_motion(self, event):
        self.tooltip_last_xy = (event.x_root, event.y_root)

    def _cancel_tooltip_schedule(self):
        if self.tooltip_after_id is not None:
            try:
                self.after_cancel(self.tooltip_after_id)
            except tk.TclError:
                pass
            self.tooltip_after_id = None

    def _show_tooltip(self, widget, text_provider):
        self.tooltip_after_id = None
        if self.tooltip_widget is not widget:
            return
        text = str(text_provider() or "").strip()
        if not text:
            return

        self._destroy_tooltip_window()
        tooltip = ctk.CTkToplevel(self)
        tooltip.withdraw()
        tooltip.overrideredirect(True)
        tooltip.attributes("-topmost", True)
        tooltip.configure(fg_color=("gray96", "gray15"))

        frame = ctk.CTkFrame(
            tooltip,
            corner_radius=6,
            border_width=1,
            border_color=("gray70", "gray35"),
            fg_color=("gray96", "gray15"),
        )
        frame.grid(row=0, column=0, sticky="nsew")
        ctk.CTkLabel(
            frame,
            text=self._wrap_tooltip_text(text),
            anchor="w",
            justify="left",
            wraplength=520,
            text_color=("gray10", "gray90"),
        ).grid(row=0, column=0, sticky="ew", padx=10, pady=8)

        tooltip.update_idletasks()
        x_root, y_root = getattr(self, "tooltip_last_xy", (widget.winfo_rootx(), widget.winfo_rooty()))
        x_pos, y_pos = self._popup_below_position(x_root + 12, y_root + 16, tooltip.winfo_reqwidth(), tooltip.winfo_reqheight())
        tooltip.geometry(f"+{x_pos}+{y_pos}")
        tooltip.deiconify()
        tooltip.lift()
        self.tooltip_window = tooltip

    def _wrap_tooltip_text(self, text: str) -> str:
        parts = [part.strip() for part in text.split("|")]
        if len(parts) <= 1:
            return text
        return "\n".join(part for part in parts if part)

    def _popup_below_position(self, x_root: int, y_root: int, width: int, height: int) -> Tuple[int, int]:
        margin = 8
        root_x = self.winfo_rootx()
        root_y = self.winfo_rooty()
        root_width = self.winfo_width()
        root_height = self.winfo_height()

        min_x = root_x + margin
        max_x = max(min_x, root_x + root_width - width - margin)
        min_y = root_y + margin
        max_y = max(min_y, root_y + root_height - height - margin)

        x_pos = min(max(min_x, x_root), max_x)
        y_pos = min(max(min_y, y_root), max_y)
        return x_pos, y_pos

    def _hide_tooltip(self, _event=None):
        self._cancel_tooltip_schedule()
        self.tooltip_widget = None
        self.tooltip_tree_cell = None
        self._destroy_tooltip_window()

    def _destroy_tooltip_window(self):
        tooltip = getattr(self, "tooltip_window", None)
        if tooltip is not None:
            try:
                tooltip.destroy()
            except tk.TclError:
                pass
            self.tooltip_window = None

    def _copy_device_id(self, device_id: str):
        self.clipboard_clear()
        self.clipboard_append(device_id)
        self._append_log(f"device_id copiado: {device_id}", tag="info")

    def _copy_device_ip(self, device_id: str):
        device = self.devices.get(device_id)
        if not device:
            self._append_log(f"Sem dados para copiar IP de {device_id}", tag="warn")
            return
        _ssid, ip, _rssi = self._device_network_info(device)
        if ip == "-":
            self._append_log(f"IP indisponivel para {device_id}; use get_state primeiro.", tag="warn")
            return
        self.clipboard_clear()
        self.clipboard_append(ip)
        self._append_log(f"IP copiado para {device_id}: {ip}", tag="info")

    def _send_context_command(self, device_id: str, command: str, tab_name: Optional[str] = None):
        if tab_name:
            self._select_device_tab(device_id, tab_name)
        self._send_cmd_to_device(device_id, command)
        if command == "get_state" and hasattr(self, "status_time_label"):
            self.status_time_label.configure(text="Atualizacao: aguardando get_state...")
        elif command == "get_technical_status" and hasattr(self, "status_time_label"):
            self.status_time_label.configure(text="Atualizacao: aguardando status_tecnico...")

    def _request_get_settings_for_device(self, device_id: str):
        self._select_device_tab(device_id, "Settings")
        self._send_cmd_to_device(device_id, "get_settings")
        if hasattr(self, "settings_status_label"):
            self.settings_status_label.configure(text="Ultima leitura: aguardando resposta...")

    def _clear_retained_for_device_id(self, device_id: str):
        self.clear_retained_device_var.set(device_id)
        self._clear_retained_for_device()

    def _select_device_tab(self, device_id: str, tab_name: str):
        self.selected_device = device_id
        if self.tree.exists(device_id):
            self.tree.selection_set(device_id)
        self.clear_retained_device_var.set(device_id)
        self._update_settings_selection_status()
        self._refresh_device_details()
        self._refresh_status_panel()
        if hasattr(self, "tabs"):
            self.tabs.set(tab_name)

    def _request_get_state_selected(self):
        if not self.selected_device:
            self._append_log("Selecione um dispositivo para get_state", tag="warn")
            return
        self._send_cmd_to_device(self.selected_device, "get_state")

    def _request_technical_status_selected(self):
        if not self.selected_device:
            self._append_log("Selecione um dispositivo para status_tecnico", tag="warn")
            return
        self._send_cmd_to_device(self.selected_device, "get_technical_status")

    def _is_status_tab_visible(self) -> bool:
        return hasattr(self, "tabs") and self.tabs.get() == "Status"

    def _on_main_tab_changed(self):
        if (
            self._is_status_tab_visible()
            and bool(self.technical_auto_update_var.get())
            and self.mqtt.connected
            and self.selected_device
            and not self._has_pending_command(self.selected_device, "get_technical_status")
        ):
            self._send_cmd_to_device(self.selected_device, "get_technical_status", log_publish=False)

    def _technical_status_auto_update_tick(self):
        interval_s = self._technical_update_interval_seconds()
        if (
            bool(self.technical_auto_update_var.get())
            and self.mqtt.connected
            and self._is_status_tab_visible()
            and self.selected_device
            and not self._has_pending_command(self.selected_device, "get_technical_status")
        ):
            self._send_cmd_to_device(self.selected_device, "get_technical_status", log_publish=False)
        self.after(interval_s * 1000, self._technical_status_auto_update_tick)

    def _technical_update_interval_seconds(self) -> int:
        try:
            value = int(str(self.technical_update_interval_var.get()).strip())
            return min(60, max(1, value))
        except ValueError:
            return 3

    def _expire_pending_commands(self):
        now = datetime.now()
        expired_cmd_ids = [
            cmd_id
            for cmd_id, pending in self.pending_cmd_by_id.items()
            if now - pending.sent_at > timedelta(seconds=PENDING_COMMAND_TIMEOUT_SEC)
        ]
        for cmd_id in expired_cmd_ids:
            pending = self.pending_cmd_by_id.pop(cmd_id, None)
            if pending:
                self._append_log(
                    f"Comando expirado sem resposta: {pending.command} para {pending.device_id} cmd_id={cmd_id}",
                    tag="warn",
                )
                if (
                    self.settings_save_in_progress
                    and pending.command == "set_settings"
                    and pending.device_id == self.settings_save_device_id
                ):
                    self._abort_settings_save("Salvar settings interrompido: comando expirou sem resposta.")

    def _has_pending_command(self, device_id: str, command: str) -> bool:
        self._expire_pending_commands()
        return any(
            pending.device_id == device_id and pending.command == command
            for pending in self.pending_cmd_by_id.values()
        )

    def _probe_known_devices(self):
        if not bool(self.auto_probe_var.get()):
            return
        now = datetime.now()
        for device_id, dev in self.devices.items():
            if dev.seen_live:
                continue
            if dev.last_probe_at and now - dev.last_probe_at < timedelta(seconds=30):
                continue
            dev.last_probe_at = now
            self._send_cmd_to_device(device_id, "get_state", log_publish=False)

    def _send_cmd(self, command: str, extra_params: Optional[Dict[str, Any]] = None):
        if not self.selected_device:
            self._append_log("Select a device before sending commands", tag="warn")
            return

        self._send_cmd_to_device(self.selected_device, command, extra_params=extra_params)

    def _ping_all_devices(self):
        if not self.devices:
            self._append_log("Nenhum dispositivo conhecido para ping", tag="warn")
            return
        if not self.mqtt.connected:
            self._append_log("MQTT desconectado; nao foi possivel enviar ping geral", tag="warn")
            return

        device_ids = sorted(self.devices.keys(), key=str.casefold)
        sent_count = 0
        for device_id in device_ids:
            before_count = len(self.pending_cmd_by_id)
            self._send_cmd_to_device(device_id, "ping", log_publish=False)
            if len(self.pending_cmd_by_id) > before_count:
                sent_count += 1
        self._append_log(f"Ping enviado para {sent_count}/{len(device_ids)} dispositivo(s)", tag="info")
        self._refresh_device_tree()

    def _send_cmd_to_device(
        self,
        device_id: str,
        command: str,
        extra_params: Optional[Dict[str, Any]] = None,
        log_publish: bool = True,
    ) -> Optional[str]:
        if not device_id:
            self._append_log("Device_id invalido", tag="warn")
            return None

        now = datetime.now()
        payload = {
            "name": command,
            "cmd_id": self._new_cmd_id(),
            "ts": now.isoformat(timespec="seconds"),
        }
        if extra_params:
            payload["args"] = extra_params

        self._expire_pending_commands()
        ok, msg = self.mqtt.publish_command(device_id, payload)
        if not ok:
            self._append_log(msg, tag="error")
            return None
        elif log_publish:
            self._append_log(f"Cmd {command} enviado para {device_id}", tag="info")
        self.pending_cmd_by_id[payload["cmd_id"]] = PendingCommand(device_id=device_id, command=command, sent_at=now)
        return payload["cmd_id"]

    def _send_set_heartbeat(self):
        try:
            interval = int(self.hb_interval_var.get().strip())
            if interval <= 0:
                raise ValueError
        except ValueError:
            self._append_log("Heartbeat interval must be a positive integer", tag="error")
            return

        self._send_cmd("set_heartbeat_interval", {"heartbeat_interval_s": interval})

    def _clear_retained_for_device(self):
        device_id = self.clear_retained_device_var.get().strip() or (self.selected_device or "")
        if not device_id:
            self._append_log("Informe ou selecione um device_id para limpar retained", tag="warn")
            return

        topics = [
            f"{BASE_TOPIC}/{device_id}/status",
            f"{BASE_TOPIC}/{device_id}/heartbeat",
            f"{BASE_TOPIC}/{device_id}/state",
            f"{BASE_TOPIC}/{device_id}/event",
            f"{BASE_TOPIC}/{device_id}/cmd/out",
            f"{BASE_TOPIC}/{device_id}/cmd/in",
        ]
        ok_count, fail_count = self.mqtt.clear_retained_topics(topics)
        self._append_log(
            f"Limpeza retained para {device_id}: ok={ok_count} fail={fail_count}",
            tag="info" if fail_count == 0 else "warn",
        )

    def _extract_payload_timestamp(self, payload_obj: Optional[Dict[str, Any]]) -> Optional[datetime]:
        if not isinstance(payload_obj, dict):
            return None
        ts_value = payload_obj.get("ts")
        if not ts_value:
            return None

        try:
            ts_text = str(ts_value).strip()
            if ts_text.endswith("Z"):
                ts_text = ts_text[:-1] + "+00:00"
            parsed = datetime.fromisoformat(ts_text)
            if parsed.tzinfo is not None:
                return parsed.astimezone().replace(tzinfo=None)
            return parsed
        except Exception:
            return None

    def _format_local_datetime(self, dt_value: Optional[datetime]) -> str:
        if not dt_value:
            return "-"
        tz_name = datetime.now().astimezone().tzname() or "local"
        return f"{dt_value.strftime('%Y-%m-%d %H:%M:%S')} {tz_name}"

    def _request_get_settings(self):
        self._send_cmd("get_settings")
        if hasattr(self, "settings_status_label"):
            self.settings_status_label.configure(text="Ultima leitura: aguardando resposta...")

    def _send_set_settings(self):
        if not self.selected_device:
            self._append_log("Select a device before saving settings", tag="warn")
            return
        if self.settings_save_in_progress:
            self._append_log("Salvar settings ja esta em andamento; aguarde a conclusao.", tag="warn")
            return
        if not self.settings_loaded:
            self._append_log("Leia os settings primeiro para evitar envio invalido", tag="warn")
            return
        if self.settings_loaded_device_id != self.selected_device:
            self._append_log(
                f"Settings carregados de {self.settings_loaded_device_id}; leia settings de {self.selected_device} antes de salvar.",
                tag="warn",
            )
            if hasattr(self, "settings_status_label"):
                self.settings_status_label.configure(
                    text=f"Settings carregados de {self.settings_loaded_device_id}; leia settings de {self.selected_device} ou abra um perfil."
                )
            return

        settings_payload = self._build_settings_payload_from_form()
        if settings_payload is None:
            return

        acr_control_candidate = settings_payload["acr_control"]
        trigger_output_candidate = settings_payload["trigger_output"]
        device_connectivity_candidate = settings_payload["device_connectivity"]
        acr_cloud = settings_payload["acr_cloud"]
        changed_sections: Dict[str, Dict[str, Any]] = {}
        changed_acr = self._diff_section("acr_control", acr_control_candidate)
        changed_trg = self._diff_section("trigger_output", trigger_output_candidate)
        changed_con = self._diff_section("device_connectivity", device_connectivity_candidate)
        changed_cloud = self._diff_section("acr_cloud", acr_cloud)

        if changed_acr:
            changed_sections["acr_control"] = changed_acr
        if changed_trg:
            changed_sections["trigger_output"] = changed_trg
        if changed_con:
            # Firmware exige policy+grace juntos quando um deles muda.
            if "apsta_policy" in changed_con or "apsta_grace_period_s" in changed_con:
                changed_con["apsta_policy"] = device_connectivity_candidate["apsta_policy"]
                changed_con["apsta_grace_period_s"] = device_connectivity_candidate["apsta_grace_period_s"]
            changed_sections["device_connectivity"] = changed_con
        if changed_cloud:
            changed_sections["acr_cloud"] = changed_cloud

        if not changed_sections:
            self._append_log("Nenhuma alteracao detectada para salvar", tag="warn")
            return

        self.settings_save_queue = self._build_settings_save_chunks(changed_sections)
        self.settings_save_device_id = self.selected_device
        self.settings_save_total = len(self.settings_save_queue)
        self.settings_save_in_progress = True
        if hasattr(self, "btn_save_settings"):
            self.btn_save_settings.configure(state="disabled")
        self._append_log(f"set_settings preparado em {self.settings_save_total} comando(s) sequenciais", tag="info")
        self._send_next_settings_save_chunk()

    def _build_settings_save_chunks(self, changed_sections: Dict[str, Dict[str, Any]]) -> list[tuple[str, Dict[str, Any]]]:
        chunks: list[tuple[str, Dict[str, Any]]] = []

        for section_name in ("acr_control", "trigger_output", "device_connectivity", "acr_cloud"):
            section_payload = changed_sections.get(section_name)
            if not section_payload:
                continue

            if section_name == "device_connectivity":
                apsta_payload: Dict[str, Any] = {}
                for key, value in section_payload.items():
                    if key in {"apsta_policy", "apsta_grace_period_s"}:
                        apsta_payload[key] = value
                    else:
                        chunks.append((section_name, {key: value}))
                if apsta_payload:
                    chunks.append((section_name, apsta_payload))
                continue

            for key, value in section_payload.items():
                chunks.append((section_name, {key: value}))

        return chunks

    def _send_next_settings_save_chunk(self):
        if not self.settings_save_in_progress or not self.settings_save_device_id:
            return
        if not self.settings_save_queue:
            self._complete_settings_save(None)
            return

        section_name, section_payload = self.settings_save_queue.pop(0)
        sent_index = self.settings_save_total - len(self.settings_save_queue)
        cmd_id = self._send_cmd_to_device(
            self.settings_save_device_id,
            "set_settings",
            {section_name: section_payload},
        )
        if not cmd_id:
            self._abort_settings_save("Salvar settings interrompido: falha ao publicar comando.")
            return
        if hasattr(self, "settings_status_label"):
            self.settings_status_label.configure(
                text=f"Salvar settings: comando {sent_index}/{self.settings_save_total} enviado ({section_name})"
            )

    def _handle_settings_save_reply(self, device_id: str, payload_obj: Dict[str, Any]) -> bool:
        if not self.settings_save_in_progress or device_id != self.settings_save_device_id:
            return False

        if payload_obj.get("ok") is not True:
            error = payload_obj.get("error") or "erro desconhecido"
            self._abort_settings_save(f"Salvar settings interrompido: {error}")
            return True

        if self.settings_save_queue:
            self._send_next_settings_save_chunk()
            return True

        result = payload_obj.get("result")
        self._complete_settings_save(result if isinstance(result, dict) else None)
        return True

    def _complete_settings_save(self, result: Optional[Dict[str, Any]]):
        total = self.settings_save_total
        completed_device_id = self.settings_save_device_id
        self.settings_save_queue = []
        self.settings_save_device_id = None
        self.settings_save_total = 0
        self.settings_save_in_progress = False
        if hasattr(self, "btn_save_settings"):
            self.btn_save_settings.configure(state="normal" if self.settings_loaded else "disabled")
        if isinstance(result, dict) and self.selected_device == completed_device_id:
            self._apply_settings_result(result, device_id=completed_device_id)
        self._append_log(f"set_settings concluido em {total} comando(s)", tag="info")
        if hasattr(self, "settings_status_label"):
            self.settings_status_label.configure(text=f"Salvar settings concluido em {total} comando(s)")

    def _abort_settings_save(self, message: str):
        self.settings_save_queue = []
        self.settings_save_device_id = None
        self.settings_save_total = 0
        self.settings_save_in_progress = False
        if hasattr(self, "btn_save_settings"):
            self.btn_save_settings.configure(state="normal" if self.settings_loaded else "disabled")
        self._append_log(message, tag="error")
        if hasattr(self, "settings_status_label"):
            self.settings_status_label.configure(text=message)

    def _build_settings_payload_from_form(self) -> Optional[Dict[str, Dict[str, Any]]]:
        self._reset_settings_field_styles()
        automatic_interval_ms = self._parse_int_field("automatic_interval_ms", self.settings_vars["automatic_interval_ms"], 0, 3600000)
        capture_duration_seconds = self._parse_int_field("capture_duration_seconds", self.settings_vars["capture_duration_seconds"], 1, 30)
        silence_threshold_rms = self._parse_int_field("silence_threshold_rms", self.settings_vars["silence_threshold_rms"], 0, 32767)
        silence_hysteresis_rms = self._parse_int_field("silence_hysteresis_rms", self.settings_vars["silence_hysteresis_rms"], 0, 32767)
        min_active_ms = self._parse_int_field("min_active_ms", self.settings_vars["min_active_ms"], 0, 30000)
        trigger_mode = self._parse_option_field("trigger_mode", self.settings_vars["trigger_mode_label"].get(), TRIGGER_MODE_LABEL_TO_VALUE)
        sta_max_retry = self._parse_int_field("sta_max_retry", self.settings_vars["sta_max_retry"], 1, 20)
        apsta_policy = self._parse_option_field("apsta_policy", self.settings_vars["apsta_policy_label"].get(), APSTA_POLICY_LABEL_TO_VALUE)
        apsta_grace_period_s = self._parse_int_field("apsta_grace_period_s", self.settings_vars["apsta_grace_period_s"], 30, 3600)
        trigger_gpio = self._parse_int_field("trigger_gpio", self.settings_vars["trigger_gpio"], -1, 48)
        trigger_active_level = self._parse_int_field("trigger_active_level", self.settings_vars["trigger_active_level"], 0, 1)
        trigger_pulse_ms = self._parse_int_field("trigger_pulse_ms", self.settings_vars["trigger_pulse_ms"], 10, 60000)

        digital_gain = self._parse_float_field("digital_gain", self.settings_vars["digital_gain"], 0.25, 16.0)
        ai_probability_threshold = self._parse_float_field("ai_probability_threshold", self.settings_vars["ai_probability_threshold"], 0.0, 100.0)

        parsed_values = [
            automatic_interval_ms,
            capture_duration_seconds,
            silence_threshold_rms,
            silence_hysteresis_rms,
            min_active_ms,
            trigger_mode,
            sta_max_retry,
            apsta_policy,
            apsta_grace_period_s,
            trigger_gpio,
            trigger_active_level,
            trigger_pulse_ms,
            digital_gain,
            ai_probability_threshold,
        ]
        if any(value is None for value in parsed_values):
            return None

        acr_control_candidate: Dict[str, Any] = {
            "automatic_enabled": bool(self.settings_vars["automatic_enabled"].get()),
            "automatic_interval_ms": automatic_interval_ms,
            "capture_duration_seconds": capture_duration_seconds,
            "digital_gain": digital_gain,
            "silence_threshold_rms": silence_threshold_rms,
            "silence_hysteresis_rms": silence_hysteresis_rms,
            "min_active_ms": min_active_ms,
            "trigger_mode": trigger_mode,
            "ai_probability_threshold": ai_probability_threshold,
        }
        trigger_output_candidate: Dict[str, Any] = {
            "enabled": bool(self.settings_vars["trigger_enabled"].get()),
            "gpio": trigger_gpio,
            "active_level": trigger_active_level,
            "pulse_ms": trigger_pulse_ms,
        }
        device_connectivity_candidate: Dict[str, Any] = {
            "sta_max_retry": sta_max_retry,
            "apsta_policy": apsta_policy,
            "apsta_grace_period_s": apsta_grace_period_s,
        }

        provisioning_ssid = self.settings_vars["provisioning_ssid"].get().strip()
        if provisioning_ssid:
            device_connectivity_candidate["provisioning_ssid"] = provisioning_ssid

        acr_cloud: Dict[str, str] = {}
        if self.settings_vars["acr_region"].get().strip():
            acr_cloud["region"] = self.settings_vars["acr_region"].get().strip()
        if self.settings_vars["acr_container_id"].get().strip():
            acr_cloud["container_id"] = self.settings_vars["acr_container_id"].get().strip()
        if self.settings_vars["acr_upload_prefix"].get().strip():
            acr_cloud["upload_prefix"] = self.settings_vars["acr_upload_prefix"].get().strip()
        if self.settings_vars["acr_bearer_token"].get().strip():
            acr_cloud["bearer_token"] = self.settings_vars["acr_bearer_token"].get().strip()
        return {
            "acr_control": acr_control_candidate,
            "trigger_output": trigger_output_candidate,
            "device_connectivity": device_connectivity_candidate,
            "acr_cloud": acr_cloud,
        }

    def _settings_profile_from_form(self) -> Optional[Dict[str, Dict[str, Any]]]:
        payload = self._build_settings_payload_from_form()
        if payload is None:
            return None
        return json.loads(json.dumps(payload))

    def _settings_profile_filename(self) -> str:
        device_id = self.settings_loaded_device_id or self.selected_device or "device"
        stamp = datetime.now().strftime("%Y%m%d_%H%M%S")
        safe_device_id = "".join(ch if ch.isalnum() or ch in "-_" else "_" for ch in device_id)
        return f"settings_{safe_device_id}_{stamp}.json"

    def _save_settings_file(self):
        if not self.settings_loaded or not self.settings_loaded_device_id:
            self._append_log("Leia settings antes de salvar arquivo", tag="warn")
            return
        profile = self._settings_profile_from_form()
        if profile is None:
            return
        file_path = filedialog.asksaveasfilename(
            title="Salvar arquivo de settings",
            defaultextension=".json",
            initialfile=self._settings_profile_filename(),
            filetypes=[("JSON", "*.json"), ("Todos os arquivos", "*.*")],
        )
        if not file_path:
            return
        document = {
            "type": "acr_mqtt_desktop_settings_profile",
            "version": 1,
            "source_device_id": self.settings_loaded_device_id,
            "saved_at": datetime.now().isoformat(timespec="seconds"),
            "settings": profile,
            "identity_fields": ["device_connectivity.provisioning_ssid", "acr_cloud.upload_prefix"],
            "identity_apply_default": False,
        }
        try:
            with open(file_path, "w", encoding="utf-8") as handle:
                json.dump(document, handle, ensure_ascii=True, indent=2, sort_keys=True)
                handle.write("\n")
        except OSError as exc:
            self._append_log(f"Falha ao salvar arquivo de settings: {exc}", tag="error")
            return
        self._append_log(f"Arquivo de settings salvo: {file_path}", tag="info")
        if hasattr(self, "settings_status_label"):
            self.settings_status_label.configure(text=f"Arquivo salvo: {Path(file_path).name}")

    def _load_settings_file(self):
        if not self.selected_device:
            self._append_log("Selecione um dispositivo destino antes de abrir arquivo de settings", tag="warn")
            return
        file_path = filedialog.askopenfilename(
            title="Abrir arquivo de settings",
            filetypes=[("JSON", "*.json"), ("Todos os arquivos", "*.*")],
        )
        if not file_path:
            return
        try:
            with open(file_path, "r", encoding="utf-8") as handle:
                document = json.load(handle)
        except (OSError, json.JSONDecodeError) as exc:
            self._append_log(f"Falha ao abrir arquivo de settings: {exc}", tag="error")
            return

        settings_payload = document.get("settings") if isinstance(document, dict) else None
        if settings_payload is None and isinstance(document, dict):
            settings_payload = document
        if not isinstance(settings_payload, dict) or not any(
            section in settings_payload
            for section in ("acr_control", "trigger_output", "device_connectivity", "acr_cloud")
        ):
            self._append_log("Arquivo de settings nao contem secoes reconhecidas", tag="error")
            return

        recall_payload = json.loads(json.dumps(settings_payload))
        connectivity = recall_payload.get("device_connectivity", {})
        if not isinstance(connectivity, dict):
            connectivity = {}
            recall_payload["device_connectivity"] = connectivity
        acr_cloud = recall_payload.get("acr_cloud", {})
        if not isinstance(acr_cloud, dict):
            acr_cloud = {}
            recall_payload["acr_cloud"] = acr_cloud
        apply_identity = bool(self.settings_apply_identity_var.get())
        has_profile_provisioning_ssid = "provisioning_ssid" in connectivity
        has_profile_upload_prefix = "upload_prefix" in acr_cloud
        if not apply_identity:
            connectivity.pop("provisioning_ssid", None)
            acr_cloud.pop("upload_prefix", None)

        target_id = self.selected_device
        preserved_provisioning_ssid = self.settings_vars["provisioning_ssid"].get().strip()
        preserved_upload_prefix = self.settings_vars["acr_upload_prefix"].get().strip()
        self._clear_settings_form_state("Arquivo: carregando settings no formulario...")
        self._apply_settings_form_values(recall_payload)
        self.settings_snapshot = {}
        if not apply_identity or not has_profile_provisioning_ssid:
            self.settings_vars["provisioning_ssid"].set(preserved_provisioning_ssid)
            self.settings_snapshot.setdefault("device_connectivity", {})["provisioning_ssid"] = preserved_provisioning_ssid
        if not apply_identity or not has_profile_upload_prefix:
            self.settings_vars["acr_upload_prefix"].set(preserved_upload_prefix)
            self.settings_snapshot.setdefault("acr_cloud", {})["upload_prefix"] = preserved_upload_prefix
        self.settings_loaded = True
        self.settings_loaded_device_id = target_id
        if apply_identity:
            self.settings_apply_identity_var.set(False)
        if hasattr(self, "btn_save_settings"):
            self.btn_save_settings.configure(state="normal")
        if hasattr(self, "btn_settings_file_save"):
            self.btn_settings_file_save.configure(state="normal")
        self._append_log(f"Arquivo de settings carregado para {target_id}: {file_path}", tag="info")
        if hasattr(self, "settings_status_label"):
            identity_text = "identidade aplicada" if apply_identity else "identidade preservada"
            self.settings_status_label.configure(
                text=(
                    f"Arquivo {Path(file_path).name} carregado para {target_id}; "
                    f"{identity_text}; revise e clique Salvar settings"
                )
            )

    def _clear_settings_form_state(self, status_text: Optional[str] = None):
        self.settings_snapshot = {}
        self.settings_loaded = False
        self.settings_loaded_device_id = None
        for key, variable in self.settings_vars.items():
            if isinstance(variable, ctk.BooleanVar):
                variable.set(False)
            else:
                variable.set("")
        if hasattr(self, "hb_interval_var"):
            self.hb_interval_var.set("60")
        self._reset_settings_field_styles()
        if hasattr(self, "btn_save_settings"):
            self.btn_save_settings.configure(state="disabled")
        if hasattr(self, "btn_settings_file_save"):
            self.btn_settings_file_save.configure(state="disabled")
        if hasattr(self, "settings_status_label"):
            self.settings_status_label.configure(
                text=status_text or "Ultima leitura: - | leia os settings antes de salvar"
            )

    def _parse_int_field(self, field_name: str, variable, min_value: int, max_value: int) -> Optional[int]:
        try:
            value = int(str(variable.get()).strip())
        except ValueError:
            self._append_log(f"Invalid integer for {field_name}", tag="error")
            self._mark_field_invalid(field_name)
            return None

        if value < min_value or value > max_value:
            self._append_log(f"{field_name} out of range ({min_value}..{max_value})", tag="error")
            self._mark_field_invalid(field_name)
            return None
        self._mark_field_valid(field_name)
        return value

    def _parse_float_field(self, field_name: str, variable, min_value: float, max_value: float) -> Optional[float]:
        try:
            value = float(str(variable.get()).strip())
        except ValueError:
            self._append_log(f"Invalid number for {field_name}", tag="error")
            self._mark_field_invalid(field_name)
            return None

        if value < min_value or value > max_value:
            self._append_log(f"{field_name} out of range ({min_value}..{max_value})", tag="error")
            self._mark_field_invalid(field_name)
            return None
        self._mark_field_valid(field_name)
        return value

    def _parse_option_field(self, field_name: str, selected_label: str, options_map: Dict[str, int]) -> Optional[int]:
        if selected_label not in options_map:
            self._append_log(f"Selecione um valor valido para {field_name}", tag="error")
            self._mark_field_invalid(field_name)
            return None
        self._mark_field_valid(field_name)
        return options_map[selected_label]

    def _mark_field_valid(self, field_name: str):
        widget = self.settings_inputs.get(field_name)
        if not widget:
            return
        try:
            widget.configure(border_color="#1f8b24")
        except Exception:
            pass

    def _mark_field_invalid(self, field_name: str):
        widget = self.settings_inputs.get(field_name)
        if not widget:
            return
        try:
            widget.configure(border_color="#d11a2a")
        except Exception:
            pass

    def _reset_settings_field_styles(self):
        for widget in self.settings_inputs.values():
            try:
                widget.configure(border_color=("gray70", "gray30"))
            except Exception:
                pass

    def _diff_section(self, section_name: str, candidate: Dict[str, Any]) -> Dict[str, Any]:
        snapshot_section = self.settings_snapshot.get(section_name, {})
        if not isinstance(snapshot_section, dict):
            snapshot_section = {}
        diff: Dict[str, Any] = {}
        for key, value in candidate.items():
            if snapshot_section.get(key) != value:
                diff[key] = value
        return diff

    def _apply_settings_form_values(self, settings_result: Dict[str, Any]):
        acr_control = settings_result.get("acr_control")
        if isinstance(acr_control, dict):
            if "automatic_enabled" in acr_control:
                self.settings_vars["automatic_enabled"].set(bool(acr_control.get("automatic_enabled")))
            self._set_var_if_present("automatic_interval_ms", acr_control.get("automatic_interval_ms"))
            self._set_var_if_present("capture_duration_seconds", acr_control.get("capture_duration_seconds"))
            self._set_var_if_present("digital_gain", acr_control.get("digital_gain"))
            self._set_var_if_present("silence_threshold_rms", acr_control.get("silence_threshold_rms"))
            self._set_var_if_present("silence_hysteresis_rms", acr_control.get("silence_hysteresis_rms"))
            self._set_var_if_present("min_active_ms", acr_control.get("min_active_ms"))
            trigger_mode_value = acr_control.get("trigger_mode")
            if trigger_mode_value is not None:
                self.settings_vars["trigger_mode_label"].set(
                    TRIGGER_MODE_VALUE_TO_LABEL.get(int(trigger_mode_value), "")
                )
            self._set_var_if_present("ai_probability_threshold", acr_control.get("ai_probability_threshold"))

        trigger_output = settings_result.get("trigger_output")
        if isinstance(trigger_output, dict):
            if "enabled" in trigger_output:
                self.settings_vars["trigger_enabled"].set(bool(trigger_output.get("enabled")))
            self._set_var_if_present("trigger_gpio", trigger_output.get("gpio"))
            self._set_var_if_present("trigger_active_level", trigger_output.get("active_level"))
            self._set_var_if_present("trigger_pulse_ms", trigger_output.get("pulse_ms"))

        device_connectivity = settings_result.get("device_connectivity")
        if isinstance(device_connectivity, dict):
            self._set_var_if_present("provisioning_ssid", device_connectivity.get("provisioning_ssid"))
            self._set_var_if_present("sta_max_retry", device_connectivity.get("sta_max_retry"))
            apsta_policy_value = device_connectivity.get("apsta_policy")
            if apsta_policy_value is not None:
                self.settings_vars["apsta_policy_label"].set(
                    APSTA_POLICY_VALUE_TO_LABEL.get(int(apsta_policy_value), "")
                )
            self._set_var_if_present("apsta_grace_period_s", device_connectivity.get("apsta_grace_period_s"))

        acr_cloud = settings_result.get("acr_cloud")
        if isinstance(acr_cloud, dict):
            self._set_var_if_present("acr_region", acr_cloud.get("region"))
            self._set_var_if_present("acr_container_id", acr_cloud.get("container_id"))
            self._set_var_if_present("acr_upload_prefix", acr_cloud.get("upload_prefix"))

        mqtt_section = settings_result.get("mqtt")
        if isinstance(mqtt_section, dict) and mqtt_section.get("heartbeat_interval_s") is not None:
            self.hb_interval_var.set(str(mqtt_section.get("heartbeat_interval_s")))

    def _apply_settings_result(self, settings_result: Dict[str, Any], device_id: Optional[str] = None):
        self._apply_settings_form_values(settings_result)
        self.settings_snapshot = json.loads(json.dumps(settings_result))
        self.settings_loaded = True
        self.settings_loaded_device_id = device_id or self.selected_device
        if hasattr(self, "btn_save_settings"):
            self.btn_save_settings.configure(state="normal")
        if hasattr(self, "btn_settings_file_save"):
            self.btn_settings_file_save.configure(state="normal")

        if hasattr(self, "settings_status_label"):
            self.settings_status_label.configure(
                text=(
                    f"Ultima leitura: {datetime.now().strftime('%Y-%m-%d %H:%M:%S')} | "
                    f"origem: {self.settings_loaded_device_id}"
                )
            )

    def _set_var_if_present(self, key: str, value: Any):
        if value is None or key not in self.settings_vars:
            return
        self.settings_vars[key].set(str(value))

    def _new_cmd_id(self) -> str:
        return f"cmd-{datetime.now().strftime('%Y%m%d%H%M%S')}-{uuid.uuid4().hex[:6]}"

    def _drain_events(self):
        try:
            while True:
                event_type, data = self.event_queue.get_nowait()
                if event_type == "connection":
                    self._on_connection_event(data)
                elif event_type == "message":
                    self._on_message_event(data)
                elif event_type == "error":
                    self._append_log(data, tag="error")
                elif event_type == "log":
                    self._append_log(data.get("text", ""), tag=data.get("level", "info"))
        except queue.Empty:
            pass
        finally:
            self.after(100, self._drain_events)

    def _on_connection_event(self, data: Dict[str, Any]):
        connected = bool(data.get("connected"))
        reason = data.get("reason", "")
        if connected:
            self.auto_connect_pending = False
            self.conn_state_var.set(reason or "Connected")
            self.conn_indicator.configure(fg_color="#1f8b24")
            self._set_connection_ui_state("connected")
            self._append_log("MQTT connected", tag="info")
            if bool(self.auto_probe_var.get()):
                self._probe_known_devices()
        else:
            self._set_disconnected_visual_state(reason or "Disconnected")
            self.pending_cmd_by_id.clear()
            self._append_log(reason or "MQTT disconnected", tag="warn")

    def _on_message_event(self, data: Dict[str, Any]):
        topic = data["topic"]
        device_id, message_type = self._parse_topic(topic)
        if not device_id or not message_type:
            return

        device = self.devices.get(device_id)
        if not device:
            device = DeviceInfo(device_id=device_id)
            self.devices[device_id] = device

        broker_time = data.get("timestamp") or datetime.now()
        is_retained = bool(data.get("retained"))

        payload_obj = data.get("payload_obj")
        payload_raw = data.get("payload_raw", "")
        is_empty_payload = not str(payload_raw).strip()

        payload_ts = self._extract_payload_timestamp(payload_obj)
        effective_ts = payload_ts or broker_time
        contact_ts = broker_time
        counts_as_presence = message_type in PRESENCE_MESSAGE_TYPES and not is_empty_payload

        if is_empty_payload:
            device.last_messages.pop(message_type, None)
            if message_type in PRESENCE_MESSAGE_TYPES and not device.seen_live:
                device.last_seen = None
                device.online = False
            self._upsert_tree_row(device)
            self._append_log(f"Payload vazio recebido em {topic}; snapshot local removido", tag="info")
            if self.selected_device == device_id:
                self._refresh_device_details()
                self._refresh_status_panel()
            return

        if is_retained:
            # Retained snapshots nao significam dispositivo online agora.
            if counts_as_presence and payload_ts and not device.seen_live:
                if not device.last_seen or payload_ts > device.last_seen:
                    device.last_seen = payload_ts
            if counts_as_presence and self.mqtt.connected and bool(self.auto_probe_var.get()) and not device.seen_live:
                now = datetime.now()
                if not device.last_probe_at or now - device.last_probe_at > timedelta(seconds=30):
                    device.last_probe_at = now
                    self._send_cmd_to_device(device_id, "get_state", log_publish=False)
        elif counts_as_presence:
            device.seen_live = True
            device.last_seen = contact_ts
            device.online = True

        self._update_device_metadata(device, payload_obj)

        device.last_messages[message_type] = MessageSnapshot(
            timestamp=effective_ts,
            topic=topic,
            payload_obj=payload_obj,
            payload_raw=payload_raw,
        )

        self._upsert_tree_row(device)

        log_line = self._build_log_line(device_id, message_type, effective_ts, payload_obj, payload_raw)
        if is_retained:
            log_line = f"[retained] {log_line}"
        log_tag = "info"
        if message_type == "cmd/out" and self.selected_device == device_id:
            log_tag = "selected_cmd"
        self._append_log(log_line, tag=log_tag)

        cmd_result_command = None
        if message_type == "cmd/out" and isinstance(payload_obj, dict):
            cmd_id = payload_obj.get("cmd_id")
            if isinstance(cmd_id, str) and cmd_id in self.pending_cmd_by_id:
                pending = self.pending_cmd_by_id.pop(cmd_id)
                cmd_result_command = pending.command
                if pending.device_id == device_id and payload_obj.get("ok") is True:
                    device.seen_live = True
                    device.last_seen = contact_ts
                    device.online = True
                    self._upsert_tree_row(device)
                if pending.command == "set_settings" and self._handle_settings_save_reply(device_id, payload_obj):
                    if self.selected_device == device_id:
                        self._refresh_device_details()
                        self._refresh_status_panel()
                    return
                if pending.device_id == device_id and pending.command == "get_state":
                    result = payload_obj.get("result")
                    if isinstance(result, dict) and payload_obj.get("ok") is True:
                        device.last_get_state_result = result
                        device.last_get_state_at = effective_ts
                elif pending.device_id == device_id and pending.command == "get_technical_status":
                    result = payload_obj.get("result")
                    if isinstance(result, dict) and payload_obj.get("ok") is True:
                        device.last_technical_status_result = result
                        device.last_technical_status_at = effective_ts
            elif (
                self.settings_save_in_progress
                and device_id == self.settings_save_device_id
                and payload_obj.get("ok") is False
                and payload_obj.get("error")
            ):
                self._abort_settings_save(f"Salvar settings interrompido: {payload_obj.get('error')}")

        if (
            message_type == "cmd/out"
            and self.selected_device == device_id
            and cmd_result_command in {"get_settings", "set_settings"}
            and isinstance(payload_obj, dict)
            and payload_obj.get("ok") is True
            and isinstance(payload_obj.get("result"), dict)
        ):
            result = payload_obj.get("result")
            if any(
                key in result
                for key in ("acr_control", "trigger_output", "device_connectivity", "acr_cloud", "mqtt")
            ):
                self._apply_settings_result(result, device_id=device_id)

        if self.selected_device == device_id:
            self._refresh_device_details()
            self._refresh_status_panel()

    def _check_offline_devices(self):
        timeout = self._heartbeat_timeout()
        now = datetime.now()
        changed = False

        for dev in self.devices.values():
            was_online = dev.online
            if dev.last_seen and now - dev.last_seen <= timedelta(seconds=timeout) and dev.seen_live:
                dev.online = True
            else:
                dev.online = False
            if dev.online != was_online:
                changed = True

        if changed:
            for dev in self.devices.values():
                self._upsert_tree_row(dev)
            if self.selected_device:
                self._refresh_device_details()
                self._refresh_status_panel()

        self.after(1000, self._check_offline_devices)

    def _heartbeat_timeout(self) -> int:
        try:
            val = int(self.heartbeat_timeout_var.get().strip())
            return max(5, val)
        except ValueError:
            return 180

    def _update_device_metadata(self, device: DeviceInfo, payload_obj: Optional[Dict[str, Any]]):
        if not isinstance(payload_obj, dict):
            return

        payloads = [payload_obj]
        result = payload_obj.get("result")
        if isinstance(result, dict):
            payloads.append(result)

        for payload in payloads:
            fw = payload.get("fw") or payload.get("firmware")
            if fw:
                device.fw = str(fw)

            session_id = payload.get("session_id") or payload.get("sid")
            if session_id:
                device.session_id = str(session_id)

    def _upsert_tree_row(self, device: DeviceInfo):
        self._schedule_device_tree_refresh()

    def _schedule_device_tree_refresh(self):
        if self.device_tree_refresh_after_id is not None:
            return
        self.device_tree_refresh_after_id = self.after(150, self._refresh_device_tree)

    def _refresh_device_age_tick(self):
        if self.devices:
            self._refresh_device_tree()
            if self.selected_device:
                self._refresh_device_details()
        self.after(1000, self._refresh_device_age_tick)

    def _on_device_filter_changed(self, *_args):
        self._refresh_device_tree()

    def _refresh_device_tree(self):
        self.device_tree_refresh_after_id = None
        if not hasattr(self, "tree"):
            return

        selected_iid = self.selected_device
        devices = list(self.devices.values())
        visible_devices = [device for device in devices if self._device_matches_current_filter(device)]
        visible_devices.sort(key=self._device_sort_key, reverse=self.tree_sort_desc)

        existing_rows = self.tree.get_children("")
        if existing_rows:
            self.tree.delete(*existing_rows)

        for device in visible_devices:
            presence_key, values = self._tree_values_for_device(device)
            self.tree.insert(
                "",
                END,
                iid=device.device_id,
                values=values,
                image=self.status_icons[presence_key],
                tags=(presence_key,),
            )

        if selected_iid and self.tree.exists(selected_iid):
            self.tree.selection_set(selected_iid)
        elif selected_iid and selected_iid not in self.devices:
            self.selected_device = None
        self._refresh_device_counts()

    def _refresh_device_counts(self):
        total = len(self.devices)
        online = sum(1 for device in self.devices.values() if device.online)
        retained = sum(1 for device in self.devices.values() if self._device_presence(device)[0] == "retained")
        attention = sum(1 for device in self.devices.values() if self._device_needs_attention(device))
        offline = sum(1 for device in self.devices.values() if self._device_presence(device)[0] == "offline")
        self.device_counts_var.set(
            f"Dispositivos: {total} | Online: {online} | Offline: {offline} | Triagem: {attention} | Retained: {retained}"
        )

    def _device_presence(self, device: DeviceInfo) -> Tuple[str, str]:
        if device.online:
            if self._pending_count_for_device(device.device_id):
                return "pending", "pendente"
            if self._device_needs_attention(device):
                return "attention", "triagem"
            return "online", "online"
        if not device.seen_live and (device.last_seen or device.last_messages):
            return "retained", "retained"
        return "offline", "offline"

    def _device_needs_attention(self, device: DeviceInfo) -> bool:
        if not device.online:
            return True
        cmd_out = self._payload_for(device, "cmd/out")
        event = self._payload_for(device, "event")
        technical = device.last_technical_status_result or {}
        return (
            cmd_out.get("ok") is False
            or bool(cmd_out.get("error"))
            or bool(event.get("error"))
            or bool(technical.get("acr_last_error"))
            or self._num(technical, "acr_consecutive_errors", 0) > 0
        )

    def _pending_count_for_device(self, device_id: str) -> int:
        self._expire_pending_commands()
        return sum(1 for pending in self.pending_cmd_by_id.values() if pending.device_id == device_id)

    def _device_matches_current_filter(self, device: DeviceInfo) -> bool:
        filter_value = self.device_filter_var.get()
        presence_key, _presence_text = self._device_presence(device)
        if filter_value == "Online" and not device.online:
            return False
        if filter_value == "Offline" and presence_key != "offline":
            return False
        if filter_value == "Triagem" and not self._device_needs_attention(device):
            return False
        if filter_value == "Retained" and presence_key != "retained":
            return False

        search = self.device_search_var.get().strip().casefold()
        if not search:
            return True

        ssid, ip, rssi = self._device_network_info(device)
        haystack = " ".join(
            [
                device.device_id,
                device.fw,
                device.session_id,
                presence_key,
                ssid,
                ip,
                rssi,
                self._device_summary(device),
            ]
        ).casefold()
        return search in haystack

    def _device_summary(self, device: DeviceInfo) -> str:
        heartbeat = self._payload_for(device, "heartbeat")
        state = self._payload_for(device, "state")
        status = self._payload_for(device, "status")
        event = self._payload_for(device, "event")
        cmd_out = self._payload_for(device, "cmd/out")
        technical = device.last_technical_status_result or {}

        parts = []
        _ssid, ip, rssi_from_state = self._device_network_info(device)
        if ip != "-":
            parts.append(f"IP {ip}")

        rssi = self._field(heartbeat, "rssi")
        if rssi == "-":
            rssi = rssi_from_state
        if rssi != "-":
            parts.append(f"RSSI {rssi}")

        vbat = self._format_summary_vbat(heartbeat, technical)
        if vbat != "-":
            parts.append(f"BAT {vbat}")

        state_text = self._field(state, "state", "runtime_state", "wifi_state")
        if state_text == "-":
            state_text = self._field(status, "status", "message", "state")
        if state_text == "-" and technical:
            state_text = self._field(technical, "acr_state", "acr_status")
        if state_text != "-":
            parts.append(self._format_state(str(state_text)))

        last_error = (
            cmd_out.get("error")
            or event.get("error")
            or technical.get("acr_last_error")
        )
        if last_error:
            parts.append(f"ERR {str(last_error)[:36]}")

        last_event = self._field(event, "event", "name", "type")
        if last_event != "-":
            parts.append(f"EV {str(last_event)[:24]}")

        pending_count = self._pending_count_for_device(device.device_id)
        if pending_count:
            parts.append(f"PEND {pending_count}")

        if not parts:
            return "sem dados live" if not device.seen_live else "-"
        return " | ".join(parts)

    def _format_summary_vbat(self, heartbeat: Dict[str, Any], technical: Dict[str, Any]) -> str:
        vbat_value = heartbeat.get("vbat") or heartbeat.get("vbat_mv")
        if vbat_value is None and isinstance(technical.get("vbat"), dict):
            vbat_value = technical["vbat"].get("vbat_mv")
        if vbat_value is None:
            return "-"
        try:
            numeric = float(vbat_value)
            if numeric > 20:
                return f"{numeric / 1000:.2f}V"
            return f"{numeric:.2f}V"
        except (TypeError, ValueError):
            return str(vbat_value)

    def _format_age(self, dt_value: Optional[datetime]) -> str:
        if not dt_value:
            return "-"
        delta = datetime.now() - dt_value
        seconds = max(0, int(delta.total_seconds()))
        if seconds < 60:
            return f"{seconds}s"
        minutes = seconds // 60
        if minutes < 60:
            return f"{minutes}m"
        hours = minutes // 60
        if hours < 48:
            return f"{hours}h"
        return f"{hours // 24}d"

    def _device_sort_key(self, device: DeviceInfo):
        col = self.tree_sort_column
        if col == "presence":
            rank = {"online": 0, "pending": 1, "attention": 2, "offline": 3, "retained": 4}
            return rank.get(self._device_presence(device)[0], 9)
        if col == "device_id":
            return device.device_id.casefold()
        if col == "age":
            return device.last_seen or datetime.min
        if col == "fw":
            return (device.fw or "").casefold()
        if col == "summary":
            return self._device_summary(device).casefold()
        return ""

    def _tree_values_for_device(self, device: DeviceInfo):
        presence_key, presence_text = self._device_presence(device)
        values = (
            presence_text,
            device.device_id,
            self._format_age(device.last_seen),
            device.fw,
            self._device_summary(device),
        )
        return presence_key, values

    def _on_tree_heading_click(self, col: str):
        if self.tree_sort_column == col:
            self.tree_sort_desc = not self.tree_sort_desc
        else:
            self.tree_sort_column = col
            self.tree_sort_desc = False
        self._refresh_tree_headings()
        self._refresh_device_tree()

    def _refresh_tree_headings(self):
        for col, label in self.tree_heading_labels.items():
            text = label
            if col == self.tree_sort_column:
                arrow = "↓" if self.tree_sort_desc else "↑"
                text = f"▶ {label} {arrow}"
            self.tree.heading(col, text=text, command=lambda c=col: self._on_tree_heading_click(c))

    def _apply_tree_sort(self):
        self._refresh_device_tree()

    def _tree_sort_key(self, iid: str):
        dev = self.devices.get(iid)
        if not dev:
            return ""
        return self._device_sort_key(dev)

    def _refresh_device_details(self):
        if not self.selected_device:
            self._set_detail("device_id", "Select a device")
            for key in self.detail_value_labels:
                if key != "device_id":
                    self._set_detail(key, "-")
            return

        device = self.devices.get(self.selected_device)
        if not device:
            self._set_detail("device_id", "No data for selected device")
            for key in self.detail_value_labels:
                if key != "device_id":
                    self._set_detail(key, "-")
            return

        heartbeat = self._payload_for(device, "heartbeat")
        state = self._payload_for(device, "state")
        status = self._payload_for(device, "status")
        event = self._payload_for(device, "event")
        cmd_out = self._payload_for(device, "cmd/out")
        ssid, ip, _rssi = self._device_network_info(device)

        self._set_detail("device_id", device.device_id)
        self._set_detail("online", "online" if device.online else "offline")
        self._set_detail("last_seen", self._format_local_datetime(device.last_seen))
        self._set_detail("wifi_ssid", ssid)
        self._set_detail("ip", ip)
        self._set_detail("fw", device.fw)
        self._set_detail("session_id", device.session_id)

        self._set_detail("hb_ts", self._field(heartbeat, "ts"))
        self._set_detail("hb_uptime", self._field(heartbeat, "uptime_s"))
        self._set_detail("hb_rssi", self._field(heartbeat, "rssi"))
        self._set_detail("hb_heap", self._field(heartbeat, "heap_free"))
        self._set_detail("hb_vbat", self._field(heartbeat, "vbat"))

        self._set_detail("state", self._field(state, "state", "runtime_state", "wifi_state"))
        self._set_detail("status", self._field(status, "status", "message", "state"))

        self._set_detail("event", self._field(event, "event", "name", "type"))
        self._set_detail("event_message", self._field(event, "message", "detail", "error"))

        self._set_detail("cmd_ts", self._field(cmd_out, "ts"))
        self._set_detail("cmd_id", self._field(cmd_out, "cmd_id"))
        self._set_detail("cmd_ok", self._field(cmd_out, "ok"))
        self._set_detail("cmd_error", self._field(cmd_out, "error"))
        self._set_detail("cmd_result", self._field(cmd_out, "result"))
        self._set_detail("state_payload", self._compact_json(state))
        self._set_detail("status_payload", self._compact_json(status))
        self._set_detail("heartbeat_payload", self._compact_json(heartbeat))

        cmd_ok_text = self.detail_value_labels["cmd_ok"].cget("text")
        if cmd_ok_text == "True":
            self.detail_value_labels["cmd_ok"].configure(text_color="#1f8b24")
        elif cmd_ok_text == "False":
            self.detail_value_labels["cmd_ok"].configure(text_color="#d11a2a")
        else:
            self.detail_value_labels["cmd_ok"].configure(text_color=("gray10", "gray90"))

    def _payload_for(self, device: DeviceInfo, msg_type: str) -> Dict[str, Any]:
        snap = device.last_messages.get(msg_type)
        if snap and isinstance(snap.payload_obj, dict):
            return snap.payload_obj
        return {}

    def _device_network_info(self, device: DeviceInfo) -> Tuple[str, str, str]:
        payloads = [
            device.last_get_state_result or {},
            self._payload_for(device, "state"),
            self._payload_for(device, "heartbeat"),
            self._payload_for(device, "status"),
        ]
        ssid = "-"
        ip = "-"
        rssi = "-"
        for payload in payloads:
            if not isinstance(payload, dict):
                continue
            if ssid == "-":
                ssid = self._field(payload, "ssid", "wifi_ssid")
            if ip == "-":
                ip = self._field(payload, "ip", "ipv4", "sta_ip", "wifi_ip")
            if rssi == "-":
                rssi = self._field(payload, "rssi")
        return ssid, ip, rssi

    def _field(self, payload: Dict[str, Any], *keys: str) -> str:
        for key in keys:
            value = payload.get(key)
            if value is not None:
                if isinstance(value, dict):
                    return json.dumps(value, ensure_ascii=True)
                return str(value)
        return "-"

    def _set_detail(self, key: str, value: Any):
        label = self.detail_value_labels.get(key)
        if not label:
            return
        label.configure(text=str(value))

    def _compact_json(self, payload: Dict[str, Any], max_len: int = 900) -> str:
        if not isinstance(payload, dict) or not payload:
            return "-"
        text = json.dumps(payload, ensure_ascii=True)
        if len(text) > max_len:
            return text[:max_len] + "..."
        return text

    def _build_log_line(
        self,
        device_id: str,
        msg_type: str,
        ts: datetime,
        payload_obj: Optional[Dict[str, Any]],
        payload_raw: str,
    ) -> str:
        base = f"[{ts.strftime('%H:%M:%S')}] {device_id} {msg_type}"

        if msg_type == "cmd/out" and isinstance(payload_obj, dict):
            cmd_id = payload_obj.get("cmd_id", "-")
            ok = payload_obj.get("ok", "-")
            error = payload_obj.get("error")
            result = payload_obj.get("result")
            return f"{base} cmd_id={cmd_id} ok={ok} error={error} result={result}"

        compact = payload_raw.strip().replace("\n", " ")
        if len(compact) > 180:
            compact = compact[:180] + "..."
        return f"{base} payload={compact}"

    def _clear_log(self):
        self.log_box.delete("1.0", END)
        self.log_line_count = 0

    def _show_log_context_menu(self, event):
        self._hide_log_context_menu()

        menu = ctk.CTkToplevel(self)
        menu.withdraw()
        menu.overrideredirect(True)
        menu.attributes("-topmost", True)
        menu.configure(fg_color=("gray96", "gray15"))

        frame = ctk.CTkFrame(
            menu,
            corner_radius=8,
            border_width=1,
            border_color=("gray70", "gray35"),
            fg_color=("gray96", "gray15"),
        )
        frame.grid(row=0, column=0, sticky="nsew")
        clear_button = ctk.CTkButton(
            frame,
            text="Clear log",
            width=116,
            height=32,
            corner_radius=6,
            fg_color="transparent",
            hover_color=("gray84", "gray28"),
            text_color=("gray10", "gray90"),
            command=self._clear_log_from_context_menu,
        )
        clear_button.grid(row=0, column=0, padx=5, pady=5)

        menu.update_idletasks()
        menu.geometry(f"+{event.x_root}+{event.y_root}")
        menu.deiconify()
        menu.lift()
        menu.focus_force()
        menu.bind("<FocusOut>", self._hide_log_context_menu, add="+")
        self.log_context_menu = menu
        return "break"

    def _hide_log_context_menu(self, _event=None):
        menu = getattr(self, "log_context_menu", None)
        if menu is not None:
            try:
                menu.destroy()
            except tk.TclError:
                pass
            self.log_context_menu = None

    def _hide_log_context_menu_if_outside(self, event):
        menu = getattr(self, "log_context_menu", None)
        if menu is None:
            return
        try:
            x0 = menu.winfo_rootx()
            y0 = menu.winfo_rooty()
            x1 = x0 + menu.winfo_width()
            y1 = y0 + menu.winfo_height()
        except tk.TclError:
            self.log_context_menu = None
            return

        if not (x0 <= event.x_root <= x1 and y0 <= event.y_root <= y1):
            self._hide_log_context_menu()

    def _clear_log_from_context_menu(self):
        self._clear_log()
        self._hide_log_context_menu()

    def _append_log(self, text: str, tag: str = "info"):
        ts = datetime.now().strftime("%H:%M:%S")
        line = f"[{ts}] {text}\n"
        self.log_box.insert(END, line, tag)
        self.log_line_count += line.count("\n")
        if self.log_line_count > MAX_LOG_LINES:
            excess = self.log_line_count - MAX_LOG_LINES
            self.log_box.delete("1.0", f"1.0 + {excess} lines")
            self.log_line_count = MAX_LOG_LINES
        self.log_box.see(END)

    def _parse_topic(self, topic: str) -> Tuple[Optional[str], Optional[str]]:
        parts = topic.split("/")
        if len(parts) < 4:
            return None, None
        if parts[0] != "v1" or parts[1] != "acr":
            return None, None

        device_id = parts[2]
        tail = parts[3:]
        if tail == ["cmd", "out"]:
            return device_id, "cmd/out"
        if len(tail) == 1 and tail[0] in {"status", "heartbeat", "state", "event"}:
            return device_id, tail[0]
        return None, None


def main():
    app = App()

    def on_close():
        try:
            app.mqtt.disconnect()
        except TclError:
            pass
        app.destroy()

    app.protocol("WM_DELETE_WINDOW", on_close)
    app.mainloop()


if __name__ == "__main__":
    main()
