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
from tkinter import END, StringVar, TclError
from tkinter import ttk


DEFAULT_BASE_TOPIC = "v1/telesystem"
APP_DIR = Path(__file__).resolve().parent
PRESENCE_MESSAGE_TYPES = {"availability", "seen", "heartbeat", "state", "event"}
PENDING_COMMAND_TIMEOUT_SEC = 30
MAX_LOG_LINES = 2000
COMMAND_OPTION_PLACEHOLDER = "selecione..."
COMMANDS_HIDDEN_FROM_COMMANDS_TAB = {"config/set", "config/reset"}


def command_args_from_inputs(arg_specs: list[Dict[str, Any]], values: Dict[str, str]) -> Dict[str, Any]:
    result: Dict[str, Any] = {}

    for spec in arg_specs:
        if not isinstance(spec, dict):
            continue
        arg_id = str(spec.get("id", "")).strip()
        if not arg_id:
            continue

        raw_value = str(values.get(arg_id, "")).strip()
        required = bool(spec.get("required"))
        if raw_value == "":
            if required:
                raise ValueError(f"{arg_id}: valor obrigatorio")
            continue

        arg_type = str(spec.get("type", "any")).strip().lower()
        value = _parse_command_arg_value(arg_id, arg_type, raw_value)
        _validate_command_arg_limits(arg_id, spec, value)
        result[arg_id] = value

    return result


def _parse_command_arg_value(arg_id: str, arg_type: str, raw_value: str) -> Any:
    if arg_type == "bool":
        lowered = raw_value.lower()
        if lowered in {"true", "1", "sim", "yes", "on"}:
            return True
        if lowered in {"false", "0", "nao", "não", "no", "off"}:
            return False
        raise ValueError(f"{arg_id}: booleano invalido")

    if arg_type == "u32":
        try:
            value = int(raw_value, 10)
        except ValueError as exc:
            raise ValueError(f"{arg_id}: inteiro invalido") from exc
        if value < 0:
            raise ValueError(f"{arg_id}: deve ser >= 0")
        return value

    if arg_type == "i32":
        try:
            return int(raw_value, 10)
        except ValueError as exc:
            raise ValueError(f"{arg_id}: inteiro invalido") from exc

    if arg_type == "string":
        return raw_value

    if arg_type == "object":
        try:
            value = json.loads(raw_value)
        except json.JSONDecodeError as exc:
            raise ValueError(f"{arg_id}: JSON invalido") from exc
        if not isinstance(value, dict):
            raise ValueError(f"{arg_id}: objeto JSON esperado")
        return value

    if arg_type == "any":
        try:
            return json.loads(raw_value)
        except json.JSONDecodeError:
            return raw_value

    return raw_value


def _validate_command_arg_limits(arg_id: str, spec: Dict[str, Any], value: Any) -> None:
    if isinstance(value, int) and not isinstance(value, bool):
        if spec.get("min") is not None and value < int(spec["min"]):
            raise ValueError(f"{arg_id}: abaixo do minimo {spec['min']}")
        if spec.get("max") is not None and value > int(spec["max"]):
            raise ValueError(f"{arg_id}: acima do maximo {spec['max']}")

    if isinstance(value, str):
        if spec.get("min_len") is not None and len(value) < int(spec["min_len"]):
            raise ValueError(f"{arg_id}: texto curto demais")
        if spec.get("max_len") is not None and len(value) > int(spec["max_len"]):
            raise ValueError(f"{arg_id}: texto longo demais")


def status_manifest_field_has_flag(field: Dict[str, Any], flag_name: str) -> bool:
    flags = field.get("flags")
    if not isinstance(flags, list):
        return False
    for item in flags:
        if isinstance(item, dict) and item.get("flag") == flag_name:
            return True
        if item == flag_name:
            return True
    return False


def split_status_manifest_fields(fields: Any) -> Tuple[list[Dict[str, Any]], list[Dict[str, Any]]]:
    general: list[Dict[str, Any]] = []
    technical: list[Dict[str, Any]] = []
    if not isinstance(fields, list):
        return general, technical

    for field in fields:
        if not isinstance(field, dict) or not field.get("id"):
            continue
        if status_manifest_field_has_flag(field, "technical"):
            technical.append(field)
        else:
            general.append(field)
    return general, technical


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
        self.base_topic = DEFAULT_BASE_TOPIC

    @staticmethod
    def normalize_base_topic(base_topic: str) -> str:
        normalized = (base_topic or DEFAULT_BASE_TOPIC).strip().strip("/")
        return normalized or DEFAULT_BASE_TOPIC

    def _topic(self, *parts: str) -> str:
        return "/".join([self.base_topic, *parts])

    def _topics_to_subscribe(self) -> list[str]:
        return [self._topic("#")]

    def connect(
        self,
        host: str,
        port: int,
        username: str,
        password: str,
        tls_enabled: bool,
        base_topic: str,
    ) -> None:
        if self.client is not None:
            self.disconnect()

        self.base_topic = self.normalize_base_topic(base_topic)
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

    def disconnect(self, emit_event: bool = True) -> None:
        if self.client is None:
            return

        client = self.client
        self.client = None
        self.connected = False
        try:
            client.disconnect()
            client.loop_stop()
        except Exception:
            pass
        if emit_event:
            self.event_queue.put(("connection", {"connected": False, "reason": "Disconnected"}))

    def publish_command(self, device_id: str, payload: Dict[str, Any]) -> Tuple[bool, str]:
        if not self.client or not self.connected:
            return False, "Not connected"

        topic = self._topic(device_id, "cmd", "in")
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
            for topic in self._topics_to_subscribe():
                result, _mid = client.subscribe(topic, qos=1)
                if result != mqtt.MQTT_ERR_SUCCESS:
                    self.event_queue.put(("log", {"level": "warn", "text": f"Subscribe failed rc={result}: {topic}"}))
            self.event_queue.put(("connection", {"connected": True, "reason": "Connected"}))
            self.event_queue.put(
                ("log", {"level": "info", "text": f"Connected and subscribed under {self.base_topic}"})
            )
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
        self.title("Jabur Consulting MQTT Control Center")
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
        self.status_manifest_labels: Dict[str, tuple[ctk.CTkLabel, ctk.CTkLabel, ctk.CTkLabel, ctk.CTkLabel]] = {}
        self.status_manifest_signature: tuple[str, ...] = ()
        self.status_technical_manifest_labels: Dict[str, tuple[ctk.CTkLabel, ctk.CTkLabel, ctk.CTkLabel, ctk.CTkLabel]] = {}
        self.status_technical_manifest_signature: tuple[str, ...] = ()
        self.status_technical_labels: Dict[str, tuple[ctk.CTkLabel, ctk.CTkLabel]] = {}
        self.status_technical_signature: tuple[str, ...] = ()
        self.status_raw_text_value = ""
        self.settings_loaded_device_id: Optional[str] = None
        self.settings_manual_config_get_device_id: Optional[str] = None
        self.settings_force_sync_device_id: Optional[str] = None
        self.settings_raw_visible = False
        self.settings_raw_text_value = ""
        self.settings_config_widgets: Dict[str, Dict[str, Any]] = {}
        self.settings_config_signature: tuple[str, ...] = ()
        self.command_manifest_labels: Dict[str, tuple[ctk.CTkLabel, ...]] = {}
        self.command_manifest_widgets: Dict[str, Dict[str, Any]] = {}
        self.command_manifest_signature: tuple[str, ...] = ()
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
        self.base_topic_var = StringVar(value=DEFAULT_BASE_TOPIC)
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
        self.status_panel_built = False
        self.commands_panel_built = False
        self.settings_panel_built = False

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
        self._labeled_entry(conn, 5, "Base topic", self.base_topic_var)
        self._labeled_entry(conn, 6, "HB timeout (s)", self.heartbeat_timeout_var)

        ctk.CTkCheckBox(conn, text="TLS", variable=self.tls_var).grid(row=7, column=0, sticky="w", padx=8, pady=(2, 8))
        ctk.CTkCheckBox(conn, text="Auto-probe online", variable=self.auto_probe_var).grid(
            row=7, column=1, sticky="w", padx=8, pady=(2, 8)
        )

        btns = ctk.CTkFrame(conn, fg_color="transparent")
        btns.grid(row=8, column=0, columnspan=2, sticky="ew", padx=8, pady=(0, 8))
        btns.grid_columnconfigure((0, 1), weight=1)
        self.btn_connect = ctk.CTkButton(btns, text="Connect", command=self._connect)
        self.btn_connect.grid(row=0, column=0, sticky="ew", padx=(0, 4))
        self.btn_disconnect = ctk.CTkButton(btns, text="Disconnect", command=self._disconnect, state="disabled")
        self.btn_disconnect.grid(row=0, column=1, sticky="ew", padx=(4, 0))

        status_frame = ctk.CTkFrame(conn)
        status_frame.grid(row=9, column=0, columnspan=2, sticky="ew", padx=8, pady=(0, 8))
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

        tab_commands = tabs.tab("Comandos")
        tab_commands.grid_columnconfigure(0, weight=1)

        tab_settings = tabs.tab("Settings")
        tab_settings.grid_columnconfigure(0, weight=1)
        tab_settings.grid_rowconfigure(0, weight=1)

        self.log_box = ctk.CTkTextbox(bottom_container, wrap="none")
        self.log_box.grid(row=0, column=0, sticky="nsew", padx=4, pady=4)
        self.log_box.tag_config("warn", foreground="#d67a00")
        self.log_box.tag_config("error", foreground="#d11a2a")
        self.log_box.tag_config("selected_cmd", foreground="#0b5ed7")
        self.log_box.tag_config("mqtt_retained", foreground="#c96a00")
        self.log_box.tag_config("mqtt_heartbeat", foreground="#0b7897")
        self.log_box.tag_config("mqtt_received", foreground="#2f855a")
        self.log_box.tag_config("mqtt_lwt", foreground="#7c3aed")
        self.log_context_menu: Optional[ctk.CTkToplevel] = None
        self.log_box.bind("<Button-3>", self._show_log_context_menu)
        self.log_box.bind("<Button-2>", self._show_log_context_menu)
        self.bind_all("<Button-1>", self._hide_log_context_menu_if_outside, add="+")
        self.bind_all("<Button-1>", self._hide_device_context_menu_if_outside, add="+")
        self.bind_all("<Escape>", self._hide_log_context_menu, add="+")
        self.bind_all("<Escape>", self._hide_device_context_menu, add="+")

        self._append_log("Application started", tag="info")

    def _ensure_status_panel(self):
        if self.status_panel_built:
            return
        self._build_status_panel(self.tabs.tab("Status"))
        self.status_panel_built = True

    def _ensure_commands_panel(self):
        if self.commands_panel_built:
            return
        self._build_commands_panel(self.tabs.tab("Comandos"))
        self.commands_panel_built = True

    def _ensure_settings_panel(self):
        if self.settings_panel_built:
            return
        self._build_settings_panel(self.tabs.tab("Settings"))
        self.settings_panel_built = True

    def _build_commands_panel(self, parent):
        parent.grid_rowconfigure(0, weight=0)
        parent.grid_rowconfigure(1, weight=0)
        parent.grid_rowconfigure(2, weight=1)
        parent.grid_columnconfigure(0, weight=1)

        frame = ctk.CTkFrame(parent)
        frame.grid(row=0, column=0, sticky="ew", padx=8, pady=(8, 4))
        for col in range(6):
            frame.grid_columnconfigure(col, weight=0)
        frame.grid_columnconfigure(6, weight=1)

        ctk.CTkLabel(frame, text="Comandos rapidos", font=ctk.CTkFont(size=16, weight="bold")).grid(
            row=0, column=0, columnspan=6, sticky="w", padx=8, pady=(6, 2)
        )
        ctk.CTkButton(frame, text="ping", width=68, command=lambda: self._send_cmd("ping")).grid(
            row=1, column=0, sticky="w", padx=(8, 3), pady=(2, 6)
        )
        ctk.CTkButton(frame, text="state", width=68, command=lambda: self._send_cmd("get_state")).grid(
            row=1, column=1, sticky="w", padx=3, pady=(2, 6)
        )
        ctk.CTkButton(frame, text="config", width=72, command=self._request_config_get).grid(
            row=1, column=2, sticky="w", padx=3, pady=(2, 6)
        )
        ctk.CTkButton(frame, text="status", width=72, command=lambda: self._send_cmd("get_technical_status")).grid(
            row=1, column=3, sticky="w", padx=3, pady=(2, 6)
        )
        ctk.CTkButton(frame, text="comandos", width=92, command=lambda: self._send_cmd("commands/get")).grid(
            row=1, column=4, sticky="w", padx=3, pady=(2, 6)
        )
        ctk.CTkButton(frame, text="reboot", width=72, command=lambda: self._send_cmd("apply_and_reboot")).grid(
            row=1, column=5, sticky="w", padx=3, pady=(2, 6)
        )

        ctk.CTkLabel(parent, text="Comandos descobertos", font=ctk.CTkFont(size=14, weight="bold")).grid(
            row=1, column=0, sticky="w", padx=8, pady=(4, 4)
        )
        self.commands_manifest_frame = ctk.CTkScrollableFrame(parent)
        self.commands_manifest_frame.grid(row=2, column=0, sticky="nsew", padx=8, pady=(0, 8))
        self.commands_manifest_frame.grid_columnconfigure(0, weight=2)
        self.commands_manifest_frame.grid_columnconfigure(1, weight=1)
        self.commands_manifest_frame.grid_columnconfigure(2, weight=3)
        self.commands_manifest_frame.grid_columnconfigure(3, weight=4)
        self.commands_manifest_frame.grid_columnconfigure(4, weight=0)
        self._refresh_commands_panel()

    def _clear_commands_manifest_view(self):
        if not hasattr(self, "commands_manifest_frame"):
            return
        for child in self.commands_manifest_frame.winfo_children():
            child.destroy()
        self.command_manifest_labels = {}
        self.command_manifest_widgets = {}
        self.command_manifest_signature = ()

    def _refresh_commands_panel(self):
        if not hasattr(self, "commands_manifest_frame"):
            return

        manifest: Dict[str, Any] = {}
        if self.selected_device and self.selected_device in self.devices:
            manifest = self._payload_for(self.devices[self.selected_device], "meta/commands")
        commands = manifest.get("commands") if isinstance(manifest, dict) else None

        if not isinstance(commands, list) or not commands:
            if self.command_manifest_signature != ("__empty__",):
                self._clear_commands_manifest_view()
                ctk.CTkLabel(
                    self.commands_manifest_frame,
                    text="Sem meta/commands recebido ainda.",
                    anchor="w",
                    text_color=("gray45", "gray65"),
                ).grid(row=0, column=0, columnspan=4, sticky="ew", padx=8, pady=8)
                self.command_manifest_signature = ("__empty__",)
            return

        normalized = [
            cmd for cmd in commands
            if isinstance(cmd, dict) and
            cmd.get("name") and
            str(cmd.get("name")) not in COMMANDS_HIDDEN_FROM_COMMANDS_TAB
        ]
        config_option_signature = ",".join(self._config_command_field_options())
        signature = tuple(
            f"{cmd.get('name')}|{json.dumps(cmd.get('args', []), sort_keys=True, ensure_ascii=True)}"
            for cmd in normalized
        ) + (f"config_options|{config_option_signature}",)
        if signature != self.command_manifest_signature:
            self._clear_commands_manifest_view()
            self.command_manifest_signature = signature
            readonly_commands = [cmd for cmd in normalized if not bool(cmd.get("mutating"))]
            mutable_commands = [cmd for cmd in normalized if bool(cmd.get("mutating"))]
            row = 0

            if readonly_commands:
                ctk.CTkLabel(
                    self.commands_manifest_frame,
                    text="Read-only",
                    anchor="w",
                    font=ctk.CTkFont(size=12, weight="bold"),
                ).grid(row=row, column=0, columnspan=5, sticky="ew", padx=6, pady=(4, 3))
                row += 1

                for group, group_commands in self._commands_by_group(readonly_commands):
                    ctk.CTkLabel(
                        self.commands_manifest_frame,
                        text=self._group_label(group),
                        anchor="w",
                        text_color=("gray35", "gray70"),
                    ).grid(row=row, column=0, columnspan=5, sticky="ew", padx=6, pady=(2, 1))
                    row += 1
                    readonly_frame = ctk.CTkFrame(self.commands_manifest_frame, fg_color="transparent")
                    readonly_frame.grid(row=row, column=0, columnspan=5, sticky="ew", padx=6, pady=(0, 6))
                    for col in range(4):
                        readonly_frame.grid_columnconfigure(col, weight=1)

                    for index, command in enumerate(group_commands):
                        name = str(command.get("name", ""))
                        button = ctk.CTkButton(
                            readonly_frame,
                            text=self._command_display_name(command),
                            height=28,
                            command=lambda cmd=command: self._send_manifest_command(cmd),
                        )
                        button.grid(row=index // 4, column=index % 4, sticky="ew", padx=3, pady=3)
                        self._bind_delayed_tooltip(button, lambda cmd=command: self._command_tooltip_text(cmd))
                        self.command_manifest_widgets[name] = {
                            "command": command,
                            "arg_specs": [],
                            "input_vars": {},
                            "send_button": button,
                        }
                    row += 1

            if mutable_commands:
                ctk.CTkLabel(
                    self.commands_manifest_frame,
                    text="Mutaveis",
                    anchor="w",
                    font=ctk.CTkFont(size=12, weight="bold"),
                ).grid(row=row, column=0, columnspan=5, sticky="ew", padx=6, pady=(4, 3))
                row += 1

            for group, group_commands in self._commands_by_group(mutable_commands):
                ctk.CTkLabel(
                    self.commands_manifest_frame,
                    text=self._group_label(group),
                    anchor="w",
                    text_color=("gray35", "gray70"),
                ).grid(row=row, column=0, columnspan=5, sticky="ew", padx=6, pady=(2, 1))
                row += 1

                for command in group_commands:
                    name = str(command.get("name", ""))
                    args = command.get("args")
                    arg_specs = args if isinstance(args, list) else []

                    command_frame = ctk.CTkFrame(self.commands_manifest_frame, fg_color="transparent")
                    command_frame.grid(row=row, column=0, columnspan=5, sticky="ew", padx=6, pady=3)
                    command_frame.grid_columnconfigure(0, weight=0)
                    command_frame.grid_columnconfigure(1, weight=1)

                    input_vars: Dict[str, StringVar] = {}
                    args_frame = ctk.CTkFrame(command_frame, fg_color="transparent")
                    args_frame.grid(row=0, column=1, sticky="ew", padx=(8, 0))
                    args_frame.grid_columnconfigure(1, weight=1)

                    arg_widgets: Dict[str, Any] = {}
                    for input_row, arg in enumerate(arg for arg in arg_specs if isinstance(arg, dict) and arg.get("id")):
                        arg_id = str(arg.get("id"))
                        input_vars[arg_id] = StringVar(value="")
                        required_mark = "*" if bool(arg.get("required")) else ""
                        arg_label = ctk.CTkLabel(args_frame, text=f"{arg_id}{required_mark}", anchor="w")
                        arg_label.grid(row=input_row, column=0, sticky="w", padx=(0, 6), pady=1)
                        self._bind_delayed_tooltip(arg_label, lambda spec=arg: self._command_arg_tooltip_text(spec))
                        option_values = self._command_arg_options(command, arg)
                        if option_values:
                            input_vars[arg_id].set(COMMAND_OPTION_PLACEHOLDER)
                            entry = ctk.CTkOptionMenu(
                                args_frame,
                                variable=input_vars[arg_id],
                                values=[COMMAND_OPTION_PLACEHOLDER, *option_values],
                                height=26,
                            )
                        else:
                            entry = ctk.CTkEntry(
                                args_frame,
                                textvariable=input_vars[arg_id],
                                placeholder_text=self._command_arg_placeholder(arg),
                                height=26,
                            )
                        entry.grid(row=input_row, column=1, sticky="ew", pady=1)
                        self._bind_delayed_tooltip(entry, lambda spec=arg: self._command_arg_tooltip_text(spec))
                        arg_widgets[arg_id] = entry

                    self._wire_config_set_command_inputs(name, input_vars, arg_widgets)

                    send_button = ctk.CTkButton(
                        command_frame,
                        text=self._command_display_name(command),
                        width=150,
                        height=28,
                        command=lambda cmd=command: self._send_manifest_command(cmd),
                    )
                    send_button.grid(row=0, column=0, sticky="nw")
                    self._bind_delayed_tooltip(send_button, lambda cmd=command: self._command_tooltip_text(cmd))
                    self.command_manifest_widgets[name] = {
                        "command": command,
                        "arg_specs": arg_specs,
                        "input_vars": input_vars,
                        "send_button": send_button,
                    }
                    row += 1

    def _commands_by_group(self, commands: list[Dict[str, Any]]) -> list[tuple[str, list[Dict[str, Any]]]]:
        grouped: Dict[str, list[Dict[str, Any]]] = {}
        for command in commands:
            group = str(command.get("group") or "general")
            grouped.setdefault(group, []).append(command)
        return [(group, grouped[group]) for group in sorted(grouped.keys(), key=str.casefold)]

    def _command_arg_options(self, command: Dict[str, Any], arg: Dict[str, Any]) -> list[str]:
        command_name = str(command.get("name") or "")
        arg_id = str(arg.get("id") or "")
        if command_name in {"config/set", "config/reset"} and arg_id == "id":
            return self._config_command_field_options()
        return []

    def _config_command_field_options(self) -> list[str]:
        if not self.selected_device:
            return []
        device = self.devices.get(self.selected_device)
        if not device:
            return []
        manifest = self._payload_for(device, "meta/config")
        fields = manifest.get("fields") if isinstance(manifest, dict) else None
        if not isinstance(fields, list):
            return []
        field_ids = []
        for field in fields:
            if not isinstance(field, dict):
                continue
            field_id = str(field.get("id") or "")
            if not field_id or self._config_has_flag(field, "read_only"):
                continue
            field_ids.append(field_id)
        return sorted(field_ids, key=str.casefold)

    def _wire_config_set_command_inputs(self, command_name: str, input_vars: Dict[str, StringVar], arg_widgets: Dict[str, Any]):
        if command_name != "config/set":
            return
        id_var = input_vars.get("id")
        id_widget = arg_widgets.get("id")
        value_widget = arg_widgets.get("value")
        if not id_var or not id_widget or not value_widget:
            return

        def update_value_hint(_selected: Optional[str] = None):
            field_id = id_var.get()
            placeholder = self._config_value_placeholder(field_id)
            current_value = self._config_current_value_for_input(field_id)
            try:
                value_widget.configure(placeholder_text=placeholder)
                if value_widget.get() != current_value:
                    value_widget.delete(0, END)
                    if current_value:
                        value_widget.insert(0, current_value)
            except TclError:
                pass

        try:
            id_widget.configure(command=update_value_hint)
        except TclError:
            return
        update_value_hint()

    def _config_value_placeholder(self, field_id: str) -> str:
        if field_id == COMMAND_OPTION_PLACEHOLDER:
            return "escolha um campo"
        field = self._config_manifest_field(field_id)
        if not field:
            return "valor"

        field_type = str(field.get("type") or "any")
        current = self._format_config_value(field, "value")
        limits = self._format_config_limits(field)
        parts = [field_type]
        if limits != "-":
            parts.append(limits)
        if current != "-":
            parts.append(f"atual {current}")
        return " | ".join(parts)

    def _config_current_value_for_input(self, field_id: str) -> str:
        field = self._config_manifest_field(field_id)
        if not field or field.get("value_hidden") is True:
            return ""

        value = field.get("value")
        if value is None or value == "":
            return ""
        if isinstance(value, bool):
            return "true" if value else "false"
        if isinstance(value, (dict, list)):
            return json.dumps(value, ensure_ascii=True)
        return str(value)

    def _command_display_name(self, command: Dict[str, Any]) -> str:
        return str(command.get("label") or command.get("name") or "-")

    def _group_label(self, group: str) -> str:
        labels = {
            "config": "Configuracao",
            "general": "Geral",
            "memory": "Memoria",
            "network": "Rede",
            "power": "Energia",
            "runtime": "Runtime",
            "status": "Status",
            "system": "Sistema",
            "time": "Tempo",
        }
        return labels.get(group, group)

    def _command_meta_text(self, command: Dict[str, Any]) -> str:
        parts = []
        if bool(command.get("mutating")):
            parts.append("mutavel")
        else:
            parts.append("read-only")
        if bool(command.get("reboot_required")):
            parts.append("reboot")
        return " | ".join(parts)

    def _command_tooltip_text(self, command: Dict[str, Any]) -> str:
        name = str(command.get("name", "-"))
        label = str(command.get("label") or name)
        description = str(command.get("description") or "")
        args_text = self._format_command_args(command.get("args"))
        parts = [label, name, self._command_meta_text(command), f"grupo: {self._group_label(str(command.get('group') or 'general'))}"]
        if description:
            parts.append(description)
        parts.append(f"args: {args_text}")
        return " | ".join(parts)

    def _command_arg_tooltip_text(self, arg: Dict[str, Any]) -> str:
        arg_id = str(arg.get("id", "-"))
        arg_type = str(arg.get("type", "any"))
        required = "obrigatorio" if bool(arg.get("required")) else "opcional"
        details = [f"{arg_id}: {arg_type}", required]
        if "min" in arg or "max" in arg:
            details.append(f"limite {arg.get('min', '-')}..{arg.get('max', '-')}")
        if "min_len" in arg or "max_len" in arg:
            details.append(f"tamanho {arg.get('min_len', '-')}..{arg.get('max_len', '-')}")
        if arg_type in {"any", "object"}:
            details.append("aceita JSON")
        return " | ".join(details)

    def _command_arg_placeholder(self, arg: Dict[str, Any]) -> str:
        arg_type = str(arg.get("type", "any"))
        if arg_type == "bool":
            return "true/false"
        if arg_type in {"i32", "u32"}:
            suffix = ""
            if "min" in arg or "max" in arg:
                suffix = f" ({arg.get('min', '-')}-{arg.get('max', '-')})"
            return f"{arg_type}{suffix}"
        if arg_type == "object":
            return '{"chave": "valor"}'
        if arg_type == "any":
            return "JSON ou texto"
        return "texto"

    def _send_manifest_command(self, command: Dict[str, Any]):
        if not self.selected_device:
            self._append_log("Selecione um dispositivo para enviar comando", tag="warn")
            return

        name = str(command.get("name", "")).strip()
        if not name:
            self._append_log("Comando sem nome no manifesto", tag="warn")
            return

        widget_data = self.command_manifest_widgets.get(name, {})
        arg_specs = widget_data.get("arg_specs", [])
        input_vars = widget_data.get("input_vars", {})
        values = {
            arg_id: "" if var.get() == COMMAND_OPTION_PLACEHOLDER else var.get()
            for arg_id, var in input_vars.items()
            if isinstance(var, StringVar)
        }

        try:
            args = command_args_from_inputs(arg_specs, values)
        except ValueError as exc:
            self._append_log(f"Argumento invalido para {name}: {exc}", tag="error")
            return

        self._send_cmd(name, args if args else None)

    def _format_command_args(self, args: Any) -> str:
        if not isinstance(args, list) or not args:
            return "-"

        parts = []
        for arg in args:
            if not isinstance(arg, dict):
                continue
            arg_id = str(arg.get("id", "-"))
            arg_type = str(arg.get("type", "any"))
            required = "obrigatorio" if bool(arg.get("required")) else "opcional"
            limits = []
            if "min" in arg or "max" in arg:
                limits.append(f"{arg.get('min', '-')}-{arg.get('max', '-')}")
            if "min_len" in arg or "max_len" in arg:
                limits.append(f"len {arg.get('min_len', 0)}-{arg.get('max_len', '-')}")
            suffix = f" ({', '.join(limits)})" if limits else ""
            parts.append(f"{arg_id}: {arg_type}, {required}{suffix}")
        return " | ".join(parts) if parts else "-"

    def _build_settings_panel(self, parent):
        parent.grid_columnconfigure(0, weight=1)
        parent.grid_rowconfigure(0, weight=0)
        parent.grid_rowconfigure(1, weight=0)
        parent.grid_rowconfigure(2, weight=2)
        parent.grid_rowconfigure(3, weight=1)

        actions = ctk.CTkFrame(parent)
        actions.grid(row=0, column=0, sticky="ew", padx=8, pady=(8, 4))
        actions.grid_columnconfigure(0, weight=0)
        actions.grid_columnconfigure(1, weight=0)
        actions.grid_columnconfigure(2, weight=0)
        actions.grid_columnconfigure(3, weight=1)
        ctk.CTkButton(actions, text="Atualizar config", command=self._request_config_get).grid(
            row=0, column=0, sticky="ew", padx=4, pady=4
        )
        self.settings_raw_toggle_button = ctk.CTkButton(
            actions,
            text="Mostrar JSON",
            width=110,
            command=self._toggle_settings_raw_text,
        )
        self.settings_raw_toggle_button.grid(row=0, column=1, sticky="ew", padx=4, pady=4)
        ctk.CTkButton(actions, text="Apply + reboot", command=lambda: self._send_cmd("apply_and_reboot")).grid(
            row=0, column=3, sticky="e", padx=4, pady=4
        )

        self.settings_status_label = ctk.CTkLabel(parent, text="Ultima leitura: -", anchor="w")
        self.settings_status_label.grid(row=1, column=0, sticky="ew", padx=8, pady=(0, 4))

        self.settings_config_frame = ctk.CTkScrollableFrame(parent)
        self.settings_config_frame.grid(row=2, column=0, sticky="nsew", padx=8, pady=(0, 8))
        self.settings_config_frame.grid_columnconfigure(0, weight=1)

        self.settings_raw_text = ctk.CTkTextbox(parent, height=180, wrap="none")
        self.settings_raw_text.insert(END, self.settings_raw_text_value or "-")
        self.settings_raw_text.configure(state="disabled")
        self._sync_settings_raw_visibility()
        self._refresh_settings_panel()

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

        ctk.CTkLabel(top, text="Status", font=ctk.CTkFont(size=16, weight="bold")).grid(
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
            ("connectivity", "Conectividade"),
            ("runtime", "Runtime"),
            ("heartbeat", "Heartbeat"),
            ("power", "Energia"),
            ("memory", "Memoria"),
            ("technical", "Tecnico"),
            ("manifest", "Manifesto"),
            ("errors", "Erros"),
        ]
        for index, (key, title) in enumerate(card_specs):
            self._status_card(self.status_body, index // 4, index % 4, key, title)

        row = 2
        ctk.CTkLabel(self.status_body, text="Status declarado", font=ctk.CTkFont(size=14, weight="bold")).grid(
            row=row, column=0, columnspan=4, sticky="w", padx=8, pady=(12, 4)
        )
        row += 1
        self.status_manifest_frame = ctk.CTkFrame(self.status_body)
        self.status_manifest_frame.grid(row=row, column=0, columnspan=4, sticky="ew", padx=8, pady=(0, 8))
        self.status_manifest_frame.grid_columnconfigure(0, weight=2)
        self.status_manifest_frame.grid_columnconfigure(1, weight=1)
        self.status_manifest_frame.grid_columnconfigure(2, weight=2)
        self.status_manifest_frame.grid_columnconfigure(3, weight=3)

        row += 1
        ctk.CTkLabel(self.status_body, text="Status tecnico declarado", font=ctk.CTkFont(size=14, weight="bold")).grid(
            row=row, column=0, columnspan=4, sticky="w", padx=8, pady=(12, 4)
        )
        row += 1
        self.status_technical_manifest_frame = ctk.CTkFrame(self.status_body)
        self.status_technical_manifest_frame.grid(row=row, column=0, columnspan=4, sticky="ew", padx=8, pady=(0, 8))
        self.status_technical_manifest_frame.grid_columnconfigure(0, weight=2)
        self.status_technical_manifest_frame.grid_columnconfigure(1, weight=1)
        self.status_technical_manifest_frame.grid_columnconfigure(2, weight=2)
        self.status_technical_manifest_frame.grid_columnconfigure(3, weight=3)

        row += 1
        ctk.CTkLabel(self.status_body, text="Snapshot tecnico", font=ctk.CTkFont(size=14, weight="bold")).grid(
            row=row, column=0, columnspan=4, sticky="w", padx=8, pady=(12, 4)
        )
        row += 1
        self.status_technical_frame = ctk.CTkFrame(self.status_body)
        self.status_technical_frame.grid(row=row, column=0, columnspan=4, sticky="ew", padx=8, pady=(0, 8))
        self.status_technical_frame.grid_columnconfigure(0, weight=2)
        self.status_technical_frame.grid_columnconfigure(1, weight=3)

        row += 1
        ctk.CTkLabel(self.status_body, text="Raw/debug", font=ctk.CTkFont(size=14, weight="bold")).grid(
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
        status_manifest_payload = self._payload_for(device, "meta/status")
        manifest_fields = status_manifest_payload.get("fields") if isinstance(status_manifest_payload, dict) else None
        has_status_manifest = isinstance(manifest_fields, list) and bool(manifest_fields)
        vbat_payload = technical_status_payload.get("vbat", {})
        power_good_payload = technical_status_payload.get("power_good", {})

        merged: Dict[str, Any] = {}
        if state_topic_payload:
            merged.update(self._flatten_dict(state_topic_payload))
        if get_state_payload:
            merged.update(self._flatten_dict(get_state_payload))
        if technical_status_payload:
            merged.update(self._flatten_dict(technical_status_payload))

        if not merged and not has_status_manifest:
            self._show_status_empty("Sem campos de status ainda. Clique em get_state ou status_tecnico.")
            self._clear_status_values()
        else:
            self._hide_status_empty()
            if merged:
                self._render_technical_status_view(
                    technical_status_payload,
                    get_state_payload,
                    state_topic_payload,
                    heartbeat_payload,
                    vbat_payload if isinstance(vbat_payload, dict) else {},
                    power_good_payload if isinstance(power_good_payload, dict) else {},
                    merged,
                )
            else:
                self._clear_status_values(clear_manifest=False)
            self._render_status_manifest_view(
                status_manifest_payload,
                heartbeat_payload,
                state_topic_payload,
                get_state_payload,
                technical_status_payload,
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

    def _clear_status_values(self, clear_manifest: bool = True):
        for key in self.status_value_labels:
            self._set_status_label(key, "--")
        if hasattr(self, "status_raw_text"):
            self._set_status_raw_text("-")
        if hasattr(self, "status_technical_frame"):
            self._render_technical_diagnostics({})
        if clear_manifest and hasattr(self, "status_manifest_frame"):
            self._clear_status_manifest_section(
                "status_manifest_frame",
                "status_manifest_labels",
                "status_manifest_signature",
            )
        if clear_manifest and hasattr(self, "status_technical_manifest_frame"):
            self._clear_status_manifest_section(
                "status_technical_manifest_frame",
                "status_technical_manifest_labels",
                "status_technical_manifest_signature",
            )

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
        vbat: Dict[str, Any],
        power_good: Dict[str, Any],
        merged: Dict[str, Any],
    ):
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
        heap_free = self._value(heartbeat, "heap_free", self._value(state_topic, "heap_free", self._value(tech, "heap_free", "--")))
        heartbeat_interval = self._value(state_topic, "heartbeat_interval_s", self._value(heartbeat, "heartbeat_interval_s", "--"))
        time_sync = self._value(state_topic, "time_synchronized", self._value(tech, "time_synchronized", "--"))
        error_text = self._first_non_empty(
            tech.get("last_error"),
            tech.get("error"),
            state_topic.get("last_error"),
            state_topic.get("error"),
        )

        self._set_status_label("card.connectivity.value", network_value)
        self._set_status_label("card.connectivity.detail", f"SSID {network_ssid}")
        self._set_status_label("card.runtime.value", self._format_uptime(uptime_value))
        self._set_status_label("card.runtime.detail", f"NTP {time_sync}")
        self._set_status_label("card.heartbeat.value", f"RSSI {network_rssi}")
        self._set_status_label("card.heartbeat.detail", f"intervalo {heartbeat_interval}s")
        self._set_status_label("card.power.value", vbat_text)
        self._set_status_label("card.power.detail", power_good_text)
        self._set_status_label("card.memory.value", heap_free)
        self._set_status_label("card.memory.detail", "heap_free")
        self._set_status_label("card.technical.value", f"{len(merged)} campos")
        self._set_status_label("card.technical.detail", "payload tecnico disponivel" if tech else "sem payload tecnico")
        self._set_status_label("card.errors.value", error_text or "sem erro")
        self._set_status_label("card.errors.detail", "campos error/last_error")

        self._render_technical_diagnostics(tech)

        raw_lines = []
        for key in sorted(merged.keys()):
            value = merged[key]
            value_text = json.dumps(value, ensure_ascii=True) if isinstance(value, (dict, list)) else str(value)
            raw_lines.append(f"{key}: {value_text}")
        self._set_status_raw_text("\n".join(raw_lines) if raw_lines else "-")

    def _render_technical_diagnostics(self, tech: Dict[str, Any]):
        if not hasattr(self, "status_technical_frame"):
            return

        if not isinstance(tech, dict) or not tech:
            if self.status_technical_signature != ("__empty__",):
                self._clear_technical_diagnostics_view()
                empty_label = ctk.CTkLabel(
                    self.status_technical_frame,
                    text="Sem payload de status_tecnico. Use o botao status_tecnico para consultar diagnostico detalhado.",
                    anchor="w",
                    text_color=("gray45", "gray65"),
                )
                empty_label.grid(row=0, column=0, columnspan=2, sticky="ew", padx=8, pady=8)
                self.status_technical_signature = ("__empty__",)
            return

        groups = self._technical_diagnostic_groups(tech)
        signature_parts = []
        for title, values in groups:
            visible_keys = tuple(sorted(key for key, value in values.items() if value not in (None, "")))
            if visible_keys:
                signature_parts.append(f"{title}|{','.join(visible_keys)}")
        signature = tuple(signature_parts)

        if signature != self.status_technical_signature:
            self._clear_technical_diagnostics_view()
            self.status_technical_signature = signature
            row = 0
            for title, values in groups:
                visible_keys = [
                    key
                    for key in sorted(values.keys())
                    if values[key] not in (None, "")
                ]
                if not visible_keys:
                    continue
                ctk.CTkLabel(
                    self.status_technical_frame,
                    text=title,
                    anchor="w",
                    font=ctk.CTkFont(weight="bold"),
                    text_color=("gray35", "gray75"),
                ).grid(row=row, column=0, columnspan=2, sticky="ew", padx=8, pady=(8, 2))
                row += 1
                for key in visible_keys:
                    key_id = f"{title}.{key}"
                    key_label = ctk.CTkLabel(self.status_technical_frame, text=key, anchor="w")
                    value_label = ctk.CTkLabel(self.status_technical_frame, text="--", anchor="w", justify="left")
                    key_label.grid(row=row, column=0, sticky="ew", padx=8, pady=2)
                    value_label.grid(row=row, column=1, sticky="ew", padx=8, pady=2)
                    self.status_technical_labels[key_id] = (key_label, value_label)
                    row += 1

        for title, values in groups:
            for key, value in values.items():
                if value in (None, ""):
                    continue
                labels = self.status_technical_labels.get(f"{title}.{key}")
                if not labels:
                    continue
                value_text = json.dumps(value, ensure_ascii=True) if isinstance(value, (dict, list)) else str(value)
                if labels[1].cget("text") != value_text:
                    labels[1].configure(text=value_text)

    def _clear_technical_diagnostics_view(self):
        for child in self.status_technical_frame.winfo_children():
            child.destroy()
        self.status_technical_labels = {}
        self.status_technical_signature = ()

    def _technical_diagnostic_groups(self, tech: Dict[str, Any]) -> list[tuple[str, Dict[str, Any]]]:
        return [
            (
                "Sistema",
                {
                    "uptime_seconds": tech.get("uptime_seconds"),
                    "time_synchronized": tech.get("time_synchronized"),
                    "heap_free": tech.get("heap_free"),
                    "heartbeat_interval_s": tech.get("heartbeat_interval_s"),
                },
            ),
            ("Power Good", tech.get("power_good") if isinstance(tech.get("power_good"), dict) else {}),
            ("VBAT", tech.get("vbat") if isinstance(tech.get("vbat"), dict) else {}),
        ]

    def _clear_status_manifest_section(self, frame_attr: str, labels_attr: str, signature_attr: str):
        frame = getattr(self, frame_attr)
        for child in frame.winfo_children():
            child.destroy()
        setattr(self, labels_attr, {})
        setattr(self, signature_attr, ())

    def _render_status_manifest_view(
        self,
        manifest: Dict[str, Any],
        heartbeat: Dict[str, Any],
        state_topic: Dict[str, Any],
        get_state: Dict[str, Any],
        technical_status: Dict[str, Any],
    ):
        if not hasattr(self, "status_manifest_frame"):
            return

        fields = manifest.get("fields") if isinstance(manifest, dict) else None
        if not isinstance(fields, list) or not fields:
            if self.status_manifest_signature != ("__empty__",):
                self._clear_status_manifest_section(
                    "status_manifest_frame",
                    "status_manifest_labels",
                    "status_manifest_signature",
                )
                ctk.CTkLabel(
                    self.status_manifest_frame,
                    text="Sem meta/status recebido ainda.",
                    anchor="w",
                    text_color=("gray45", "gray65"),
                ).grid(row=0, column=0, columnspan=4, sticky="ew", padx=8, pady=8)
                self.status_manifest_signature = ("__empty__",)
            if self.status_technical_manifest_signature != ("__empty__",):
                self._clear_status_manifest_section(
                    "status_technical_manifest_frame",
                    "status_technical_manifest_labels",
                    "status_technical_manifest_signature",
                )
                ctk.CTkLabel(
                    self.status_technical_manifest_frame,
                    text="Sem meta/status tecnico recebido ainda.",
                    anchor="w",
                    text_color=("gray45", "gray65"),
                ).grid(row=0, column=0, columnspan=4, sticky="ew", padx=8, pady=8)
                self.status_technical_manifest_signature = ("__empty__",)
            return

        general_items, technical_items = split_status_manifest_fields(fields)
        general_count = self._render_status_manifest_section(
            "status_manifest_frame",
            "status_manifest_labels",
            "status_manifest_signature",
            "Sem campos operacionais em meta/status.",
            general_items,
            heartbeat,
            state_topic,
            get_state,
            technical_status,
        )
        technical_count = self._render_status_manifest_section(
            "status_technical_manifest_frame",
            "status_technical_manifest_labels",
            "status_technical_manifest_signature",
            "Sem campos tecnicos declarados em meta/status.",
            technical_items,
            heartbeat,
            state_topic,
            get_state,
            technical_status,
        )

        self._set_status_label("card.manifest.value", f"{general_count + technical_count} campos")
        revision = manifest.get("registry_revision", "-") if isinstance(manifest, dict) else "-"
        self._set_status_label("card.manifest.detail", f"{general_count} status | {technical_count} tecnicos | rev {revision}")

    def _render_status_manifest_section(
        self,
        frame_attr: str,
        labels_attr: str,
        signature_attr: str,
        empty_text: str,
        status_items: list[Dict[str, Any]],
        heartbeat: Dict[str, Any],
        state_topic: Dict[str, Any],
        get_state: Dict[str, Any],
        technical_status: Dict[str, Any],
    ) -> int:
        frame = getattr(self, frame_attr)
        labels_by_id = getattr(self, labels_attr)
        current_signature = getattr(self, signature_attr)

        signature = tuple(
            f"{field.get('group') or 'general'}|{field.get('id')}"
            for field in status_items
        )
        if not signature:
            if current_signature != ("__empty__",):
                self._clear_status_manifest_section(frame_attr, labels_attr, signature_attr)
                ctk.CTkLabel(
                    frame,
                    text=empty_text,
                    anchor="w",
                    text_color=("gray45", "gray65"),
                ).grid(row=0, column=0, columnspan=4, sticky="ew", padx=8, pady=8)
                setattr(self, signature_attr, ("__empty__",))
            return 0

        if signature != current_signature:
            self._clear_status_manifest_section(frame_attr, labels_attr, signature_attr)
            labels_by_id = getattr(self, labels_attr)
            setattr(self, signature_attr, signature)
            row = 0
            current_group = None
            for field in sorted(status_items, key=lambda item: (str(item.get("group") or "general"), str(item.get("id") or ""))):
                field_id = str(field.get("id") or "")
                group = str(field.get("group") or "general")
                if group != current_group:
                    ctk.CTkLabel(
                        frame,
                        text=self._group_label(group),
                        anchor="w",
                        font=ctk.CTkFont(weight="bold"),
                        text_color=("gray35", "gray75"),
                    ).grid(row=row, column=0, columnspan=4, sticky="ew", padx=8, pady=(8, 2))
                    row += 1
                    current_group = group
                row_labels = []
                for col in range(4):
                    label = ctk.CTkLabel(
                        frame,
                        text="--",
                        anchor="w",
                        justify="left",
                        wraplength=260 if col in (0, 3) else 180,
                    )
                    label.grid(row=row, column=col, sticky="ew", padx=8, pady=2)
                    self._bind_delayed_tooltip(label, lambda item=field: self._status_field_tooltip_text(item))
                    row_labels.append(label)
                labels_by_id[field_id] = tuple(row_labels)
                row += 1

        values = self._manifest_value_sources(heartbeat, state_topic, get_state, technical_status)
        for field in status_items:
            if not isinstance(field, dict):
                continue
            field_id = str(field.get("id") or "")
            if not field_id:
                continue
            unit = str(field.get("unit") or "")
            value = values.get(field_id, "--")
            value_text = self._format_manifest_value(value, unit)
            type_text = str(field.get("type") or "-")
            flags_text = self._format_manifest_flags(field.get("flags"))
            label_text = str(field.get("label") or field_id)
            labels = labels_by_id.get(field_id)
            if not labels:
                continue

            for label, text in zip(labels, (label_text, type_text, value_text, flags_text)):
                if label.cget("text") != text:
                    label.configure(text=text)

        return len(signature)

    def _manifest_value_sources(
        self,
        heartbeat: Dict[str, Any],
        state_topic: Dict[str, Any],
        get_state: Dict[str, Any],
        technical_status: Dict[str, Any],
    ) -> Dict[str, Any]:
        values: Dict[str, Any] = {}
        for payload in (technical_status, get_state, state_topic, heartbeat):
            if isinstance(payload, dict):
                values.update(self._flatten_dict(payload))
        return values

    def _status_field_tooltip_text(self, field: Dict[str, Any]) -> str:
        field_id = str(field.get("id") or "-")
        label = str(field.get("label") or field_id)
        description = str(field.get("description") or "")
        group = self._group_label(str(field.get("group") or "general"))
        field_type = str(field.get("type") or "-")
        unit = str(field.get("unit") or "")
        parts = [label, field_id, f"grupo: {group}", f"tipo: {field_type}"]
        if unit:
            parts.append(f"unidade: {unit}")
        if description:
            parts.append(description)
        return " | ".join(parts)

    def _format_manifest_value(self, value: Any, unit: str) -> str:
        if value is None or value == "":
            return "--"
        if isinstance(value, bool):
            text = "true" if value else "false"
        elif isinstance(value, (dict, list)):
            text = json.dumps(value, ensure_ascii=True)
        else:
            text = str(value)
        if unit and text != "--":
            return f"{text} {unit}"
        return text

    def _format_manifest_flags(self, flags: Any) -> str:
        if not isinstance(flags, list):
            return "-"
        names = []
        for item in flags:
            if isinstance(item, dict) and item.get("flag"):
                names.append(str(item.get("flag")))
            elif isinstance(item, str):
                names.append(item)
        return ", ".join(names) if names else "-"

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

    def _first_non_empty(self, *values: Any) -> str:
        for value in values:
            if value not in (None, ""):
                return str(value)
        return ""

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
            ("Availability", "availability"),
            ("Event", "event"),
            ("Event message", "event_message"),
            ("Cmd/out ts", "cmd_ts"),
            ("Cmd ID", "cmd_id"),
            ("Cmd OK", "cmd_ok"),
            ("Cmd error", "cmd_error"),
            ("Cmd result", "cmd_result"),
            ("State payload", "state_payload"),
            ("Availability payload", "availability_payload"),
            ("Seen payload", "seen_payload"),
            ("Heartbeat payload", "heartbeat_payload"),
            ("Config manifest payload", "config_manifest_payload"),
            ("Status manifest payload", "status_manifest_payload"),
            ("Commands manifest payload", "commands_manifest_payload"),
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
            path = APP_DIR / filename
            if path.exists():
                try:
                    config_data = json.loads(path.read_text(encoding="utf-8"))
                    self._append_log(f"Loaded configuration from {path}", tag="info")
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
        self.base_topic_var.set(str(mqtt_cfg.get("base_topic", self.base_topic_var.get())))
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
            base_topic=self.base_topic_var.get().strip(),
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
            self._refresh_active_device_tab()
            return

        self.selected_device = selection[0]
        self.clear_retained_device_var.set(self.selected_device)
        self._update_settings_selection_status()
        self._refresh_device_details()
        self._refresh_active_device_tab()

    def _update_settings_selection_status(self):
        if not hasattr(self, "settings_status_label"):
            return
        if self.settings_loaded_device_id and self.selected_device != self.settings_loaded_device_id:
            previous_device = self.settings_loaded_device_id
            self.settings_loaded_device_id = None
            self._set_settings_raw_text("-")
            self.settings_status_label.configure(
                text=f"Settings de {previous_device} limpos ao trocar device. Use config/get no device atual."
            )

    def _refresh_settings_panel(self, force_reload: bool = False):
        if not hasattr(self, "settings_config_frame"):
            return

        if not self.selected_device:
            self._render_config_manifest_view(None)
            self._set_settings_raw_text("-")
            if hasattr(self, "settings_status_label"):
                self.settings_status_label.configure(text="Selecione um dispositivo para visualizar settings.")
            return

        device = self.devices.get(self.selected_device)
        if not device:
            self._render_config_manifest_view(None)
            self._set_settings_raw_text("-")
            if hasattr(self, "settings_status_label"):
                self.settings_status_label.configure(text="Dispositivo sem dados de settings.")
            return

        config_manifest = self._payload_for(device, "meta/config")
        should_force_reload = force_reload or self.settings_force_sync_device_id == self.selected_device
        self._render_config_manifest_view(config_manifest, force_reload=should_force_reload)
        if should_force_reload:
            self.settings_force_sync_device_id = None
        if config_manifest and self.settings_loaded_device_id != self.selected_device:
            self._set_settings_raw_text(json.dumps(config_manifest, ensure_ascii=True, indent=2, sort_keys=True))

    def _clear_config_manifest_view(self):
        for child in self.settings_config_frame.winfo_children():
            child.destroy()
        self.settings_config_widgets = {}
        self.settings_config_signature = ()

    def _render_config_manifest_view(self, manifest: Optional[Dict[str, Any]], force_reload: bool = False):
        if not hasattr(self, "settings_config_frame"):
            return

        fields = manifest.get("fields") if isinstance(manifest, dict) else None
        if not isinstance(fields, list) or not fields:
            if self.settings_config_signature != ("__empty__",):
                self._clear_config_manifest_view()
                ctk.CTkLabel(
                    self.settings_config_frame,
                    text="Sem meta/config recebido ainda.",
                    anchor="w",
                    text_color=("gray45", "gray65"),
                ).grid(row=0, column=0, sticky="ew", padx=8, pady=8)
                self.settings_config_signature = ("__empty__",)
            return

        valid_fields = [field for field in fields if isinstance(field, dict) and field.get("id")]
        sorted_fields = sorted(valid_fields, key=self._config_field_sort_key)
        signature = tuple(self._config_field_signature(field) for field in sorted_fields)
        if signature != self.settings_config_signature:
            self._clear_config_manifest_view()
            self.settings_config_signature = signature
            current_group = None
            row = 0
            for field in sorted_fields:
                group = self._config_field_group(field)
                if group != current_group:
                    current_group = group
                    ctk.CTkLabel(
                        self.settings_config_frame,
                        text=self._group_label(group),
                        anchor="w",
                        font=ctk.CTkFont(size=14, weight="bold"),
                        text_color=("gray35", "gray75"),
                    ).grid(row=row, column=0, sticky="ew", padx=8, pady=(10, 2))
                    row += 1

                field_id = str(field.get("id") or "")
                field_frame = ctk.CTkFrame(self.settings_config_frame)
                field_frame.grid(row=row, column=0, sticky="ew", padx=8, pady=2)
                field_frame.grid_columnconfigure(0, weight=0, minsize=210)
                field_frame.grid_columnconfigure(1, weight=1)
                field_frame.grid_columnconfigure((2, 3, 4), weight=0)

                title_label = ctk.CTkLabel(
                    field_frame,
                    text=self._config_field_display_name(field),
                    anchor="w",
                    justify="left",
                    font=ctk.CTkFont(weight="bold"),
                )
                title_label.grid(row=0, column=0, sticky="ew", padx=(8, 8), pady=6)

                input_widget, input_var = self._create_settings_input_widget(field, field_frame)
                input_widget.grid(row=0, column=1, sticky="ew", padx=(0, 8), pady=6)

                dirty_label = ctk.CTkLabel(
                    field_frame,
                    text="",
                    width=64,
                    anchor="w",
                    text_color="#d9822b",
                )
                dirty_label.grid(row=0, column=2, sticky="w", padx=(0, 6), pady=6)

                set_button = ctk.CTkButton(
                    field_frame,
                    text="Salvar",
                    width=70,
                    command=lambda fid=field_id: self._send_config_set_from_field(fid),
                )
                set_button.grid(row=0, column=3, sticky="ew", padx=(0, 6), pady=6)
                reset_button = ctk.CTkButton(
                    field_frame,
                    text="Reset",
                    width=64,
                    command=lambda fid=field_id: self._send_config_reset_from_field(fid),
                )
                reset_button.grid(row=0, column=4, sticky="ew", padx=(0, 8), pady=6)
                tooltip = lambda fid=field_id: self._settings_field_tooltip(fid)
                for widget in (field_frame, title_label, input_widget, dirty_label, set_button, reset_button):
                    self._bind_delayed_tooltip(widget, tooltip)
                input_var.trace_add("write", lambda *_args, fid=field_id: self._refresh_settings_dirty_indicator(fid))
                self.settings_config_widgets[field_id] = {
                    "frame": field_frame,
                    "title_label": title_label,
                    "dirty_label": dirty_label,
                    "input": input_widget,
                    "input_var": input_var,
                    "input_kind": self._settings_input_kind(field),
                    "set_button": set_button,
                    "reset_button": reset_button,
                    "last_loaded_value": None,
                }
                row += 1

        for field in sorted_fields:
            if not isinstance(field, dict):
                continue
            field_id = str(field.get("id") or "")
            if not field_id:
                continue
            widgets = self.settings_config_widgets.get(field_id)
            if not widgets:
                continue
            title_label = widgets.get("title_label")
            input_widget = widgets.get("input")
            set_button = widgets.get("set_button")
            reset_button = widgets.get("reset_button")

            apply_text = self._format_config_application(field)
            if title_label:
                display_name = self._config_field_display_name(field)
                if title_label.cget("text") != display_name:
                    title_label.configure(text=display_name)
                if "reboot" in apply_text:
                    title_label.configure(text_color="#d9822b")
                elif "runtime" in apply_text:
                    title_label.configure(text_color="#1f8b24")
                else:
                    title_label.configure(text_color=("gray10", "gray90"))

            editable = not self._config_has_flag(field, "read_only") and not field.get("value_hidden")
            if input_widget and set_button and reset_button:
                self._sync_settings_input_widget(field, widgets, editable, force_reload=force_reload)
                self._refresh_settings_dirty_indicator(field_id)
                button_state = "normal" if editable else "disabled"
                if set_button.cget("state") != button_state:
                    set_button.configure(state=button_state)
                reset_state = "normal" if not self._config_has_flag(field, "read_only") else "disabled"
                if reset_button.cget("state") != reset_state:
                    reset_button.configure(state=reset_state)

        if hasattr(self, "settings_status_label"):
            revision = manifest.get("registry_revision", "-") if isinstance(manifest, dict) else "-"
            self.settings_status_label.configure(
                text=f"meta/config: {len(signature)} campos | revision {revision} | device: {self.selected_device}"
            )

    def _config_field_signature(self, field: Dict[str, Any]) -> str:
        field_id = str(field.get("id") or "")
        field_type = str(field.get("type") or "any")
        flags = field.get("flags")
        flag_names = []
        if isinstance(flags, list):
            for item in flags:
                if isinstance(item, dict):
                    flag_names.append(str(item.get("flag") or ""))
                else:
                    flag_names.append(str(item))
        return "|".join(
            (
                self._config_field_group(field),
                field_id,
                field_type,
                ",".join(sorted(flag for flag in flag_names if flag)),
                "hidden" if field.get("value_hidden") else "visible",
                json.dumps(field.get("choices") or [], sort_keys=True, ensure_ascii=True),
            )
        )

    def _config_field_sort_key(self, field: Dict[str, Any]) -> Tuple[str, str]:
        return (self._config_field_group(field).casefold(), str(field.get("id") or "").casefold())

    def _config_field_group(self, field: Dict[str, Any]) -> str:
        group = str(field.get("group") or "").strip()
        if group:
            return group
        field_id = str(field.get("id") or "")
        if "." in field_id:
            return field_id.split(".", 1)[0]
        return "general"

    def _config_field_display_name(self, field: Dict[str, Any]) -> str:
        field_id = str(field.get("id") or "-")
        group = self._config_field_group(field)
        prefix = f"{group}."
        if field_id.startswith(prefix):
            return field_id[len(prefix):]
        return field_id

    def _create_settings_input_widget(self, field: Dict[str, Any], parent: Any) -> Tuple[Any, Any]:
        input_kind = self._settings_input_kind(field)
        if input_kind == "bool":
            var = ctk.BooleanVar(value=False)
            widget = ctk.CTkSwitch(parent, text="", variable=var, width=44)
            return widget, var
        if input_kind == "enum":
            var = StringVar(value="")
            widget = ctk.CTkOptionMenu(
                parent,
                variable=var,
                values=self._config_enum_options(field),
                height=28,
            )
            return widget, var

        var = StringVar(value="")
        widget = ctk.CTkEntry(parent, textvariable=var, width=220)
        return widget, var

    def _settings_input_kind(self, field: Dict[str, Any]) -> str:
        field_type = str(field.get("type") or "")
        if field_type == "bool":
            return "bool"
        if field_type == "enum" and self._config_enum_options(field):
            return "enum"
        return "entry"

    def _config_enum_options(self, field: Dict[str, Any]) -> list[str]:
        choices = field.get("choices")
        if not isinstance(choices, list):
            return []
        options = []
        for choice in choices:
            if not isinstance(choice, dict):
                continue
            if "value" not in choice:
                continue
            value = str(choice.get("value"))
            label = str(choice.get("label") or value)
            options.append(f"{label} ({value})")
        return options

    def _config_enum_value_to_option(self, field: Dict[str, Any], value: str) -> str:
        choices = field.get("choices")
        if isinstance(choices, list):
            for choice in choices:
                if not isinstance(choice, dict) or "value" not in choice:
                    continue
                choice_value = str(choice.get("value"))
                if choice_value == str(value):
                    label = str(choice.get("label") or choice_value)
                    return f"{label} ({choice_value})"
        return str(value)

    def _config_enum_option_to_value(self, option: str) -> str:
        text = str(option).strip()
        if text.endswith(")") and "(" in text:
            return text.rsplit("(", 1)[1][:-1].strip()
        return text

    def _settings_field_tooltip(self, field_id: str) -> str:
        field = self._config_manifest_field(field_id)
        if not field:
            return field_id

        flags = field.get("flags")
        flag_names = []
        if isinstance(flags, list):
            for item in flags:
                if isinstance(item, dict):
                    flag = str(item.get("flag") or "")
                else:
                    flag = str(item)
                if flag:
                    flag_names.append(flag)

        parts = [
            f"id: {field.get('id') or '-'}",
            f"tipo: {field.get('type') or 'any'}",
            f"valor: {self._format_config_value(field, 'value')}",
            f"default: {self._format_config_value(field, 'default')}",
            f"origem: {field.get('source') or '-'}",
            f"limites: {self._format_config_limits(field)}",
            f"opcoes: {', '.join(self._config_enum_options(field)) if self._config_enum_options(field) else '-'}",
            f"aplicacao: {self._format_config_application(field)}",
            f"flags: {', '.join(flag_names) if flag_names else '-'}",
        ]
        return " | ".join(parts)

    def _sync_settings_input_widget(
        self,
        field: Dict[str, Any],
        widgets: Dict[str, Any],
        editable: bool,
        force_reload: bool = False,
    ):
        input_widget = widgets.get("input")
        input_var = widgets.get("input_var")
        if not input_widget or input_var is None:
            return

        field_type = str(field.get("type") or "")
        input_kind = str(widgets.get("input_kind") or self._settings_input_kind(field))
        desired_value = self._config_value_for_input(field)
        if input_kind == "enum":
            desired_value = self._config_enum_value_to_option(field, desired_value)
        state = "normal" if editable else "disabled"
        if input_widget.cget("state") != state:
            input_widget.configure(state=state)

        if input_kind == "bool":
            desired_bool = str(desired_value).lower() in {"true", "1", "sim", "yes", "on"}
            current_bool = bool(input_var.get())
            current_value = "true" if current_bool else "false"
            desired_value = "true" if desired_bool else "false"
            if force_reload:
                if current_bool != desired_bool:
                    input_var.set(desired_bool)
                widgets["last_loaded_value"] = desired_value
                self._refresh_settings_dirty_indicator(str(field.get("id") or ""))
                return
            if current_value == desired_value:
                widgets["last_loaded_value"] = desired_value
                self._refresh_settings_dirty_indicator(str(field.get("id") or ""))
                return
            last_loaded = widgets.get("last_loaded_value")
            should_load = last_loaded is None or current_value == last_loaded
            if should_load and current_bool != desired_bool:
                input_var.set(desired_bool)
                current_value = desired_value
            if should_load:
                widgets["last_loaded_value"] = desired_value
            self._refresh_settings_dirty_indicator(str(field.get("id") or ""))
            return

        placeholder = "<secret>" if field.get("value_hidden") else desired_value
        if input_kind == "entry" and input_widget.cget("placeholder_text") != placeholder:
            input_widget.configure(placeholder_text=placeholder)

        current_text = input_var.get()
        last_loaded = widgets.get("last_loaded_value")
        if force_reload:
            if current_text != desired_value:
                input_var.set(desired_value)
            widgets["last_loaded_value"] = desired_value
            self._refresh_settings_dirty_indicator(str(field.get("id") or ""))
            return
        if current_text == desired_value:
            widgets["last_loaded_value"] = desired_value
            self._refresh_settings_dirty_indicator(str(field.get("id") or ""))
            return
        should_load = current_text == "" or current_text == last_loaded
        if should_load and current_text != desired_value:
            input_var.set(desired_value)
            widgets["last_loaded_value"] = desired_value
        self._refresh_settings_dirty_indicator(str(field.get("id") or ""))

    def _refresh_settings_dirty_indicator(self, field_id: str):
        widgets = self.settings_config_widgets.get(field_id)
        if not widgets:
            return

        dirty_label = widgets.get("dirty_label")
        if not dirty_label:
            return

        dirty = self._settings_field_is_dirty(field_id, widgets)
        label_text = "alterado" if dirty else ""
        if dirty_label.cget("text") != label_text:
            dirty_label.configure(text=label_text)

    def _settings_field_is_dirty(self, field_id: str, widgets: Dict[str, Any]) -> bool:
        field = self._config_manifest_field(field_id)
        input_var = widgets.get("input_var")
        last_loaded = widgets.get("last_loaded_value")
        if not field or input_var is None or last_loaded is None:
            return False

        if self._config_has_flag(field, "read_only") or field.get("value_hidden"):
            return False

        field_type = str(field.get("type") or "")
        if field_type == "bool":
            current = "true" if bool(input_var.get()) else "false"
        elif field_type == "enum" and self._config_enum_options(field):
            current = self._config_enum_option_to_value(str(input_var.get()))
            last_loaded = self._config_enum_option_to_value(str(last_loaded))
        else:
            current = str(input_var.get())
        return current != str(last_loaded)

    def _config_value_for_input(self, field: Dict[str, Any]) -> str:
        if field.get("value_hidden") is True:
            return ""

        value = field.get("value")
        if value is None or value == "":
            return ""
        if isinstance(value, bool):
            return "true" if value else "false"
        if isinstance(value, (dict, list)):
            return json.dumps(value, ensure_ascii=True)
        return str(value)

    def _format_config_value(self, field: Dict[str, Any], key: str) -> str:
        if field.get(f"{key}_hidden") is True:
            return "<secret>"
        value = field.get(key)
        if value is None or value == "":
            return "-"
        if isinstance(value, bool):
            return "true" if value else "false"
        if isinstance(value, (dict, list)):
            return json.dumps(value, ensure_ascii=True)
        return str(value)

    def _format_config_limits(self, field: Dict[str, Any]) -> str:
        if field.get("min") is not None or field.get("max") is not None:
            return f"{field.get('min', '-')}..{field.get('max', '-')}"
        if field.get("min_len") is not None or field.get("max_len") is not None:
            return f"len {field.get('min_len', '-')}..{field.get('max_len', '-')}"
        return "-"

    def _config_has_flag(self, field: Dict[str, Any], flag_name: str) -> bool:
        flags = field.get("flags")
        if not isinstance(flags, list):
            return False
        for item in flags:
            if isinstance(item, dict) and item.get("flag") == flag_name:
                return True
            if item == flag_name:
                return True
        return False

    def _format_config_application(self, field: Dict[str, Any]) -> str:
        runtime = self._config_has_flag(field, "runtime_apply")
        reboot = self._config_has_flag(field, "reboot_required")
        if runtime and reboot:
            return "runtime + reboot"
        if runtime:
            return "runtime"
        if reboot:
            return "reboot"
        return "armazenado"

    def _config_manifest_field(self, field_id: str) -> Optional[Dict[str, Any]]:
        if not self.selected_device:
            return None
        device = self.devices.get(self.selected_device)
        if not device:
            return None
        manifest = self._payload_for(device, "meta/config")
        fields = manifest.get("fields") if isinstance(manifest, dict) else None
        if not isinstance(fields, list):
            return None
        for field in fields:
            if isinstance(field, dict) and str(field.get("id") or "") == field_id:
                return field
        return None

    def _parse_config_entry_value(self, field: Dict[str, Any], text: str) -> Tuple[bool, Any, str]:
        field_type = str(field.get("type") or "")
        raw = text.strip()
        if field_type == "bool":
            lowered = raw.lower()
            if lowered in {"true", "1", "sim", "yes", "on"}:
                return True, True, ""
            if lowered in {"false", "0", "nao", "não", "no", "off"}:
                return True, False, ""
            return False, None, "Valor booleano deve ser true/false"
        if field_type in {"i32", "enum", "u32"}:
            try:
                value = int(raw)
            except ValueError:
                return False, None, "Valor numerico deve ser inteiro"
            if field_type == "u32" and value < 0:
                return False, None, "Valor u32 nao pode ser negativo"
            return True, value, ""
        if field_type == "string":
            if raw == "":
                return False, None, "Valor string nao pode ser vazio"
            return True, raw, ""
        return False, None, f"Tipo nao suportado: {field_type or '-'}"

    def _send_config_set_from_field(self, field_id: str):
        if not self.selected_device:
            self._append_log("Selecione um dispositivo para config/set", tag="warn")
            return
        field = self._config_manifest_field(field_id)
        widgets = self.settings_config_widgets.get(field_id)
        if not field or not widgets:
            self._append_log(f"Campo de config indisponivel: {field_id}", tag="warn")
            return
        if self._config_has_flag(field, "read_only"):
            self._append_log(f"Campo read_only nao pode ser alterado: {field_id}", tag="warn")
            return

        raw_value = self._settings_widget_text_value(field, widgets)
        ok, value, error = self._parse_config_entry_value(field, raw_value)
        if not ok:
            self._append_log(f"{field_id}: {error}", tag="error")
            return

        self._send_cmd("config/set", {"id": field_id, "value": value})
        widgets["last_loaded_value"] = self._config_value_for_input(field)
        if hasattr(self, "settings_status_label"):
            self.settings_status_label.configure(text=f"config/set enviado para {field_id}; aguardando ACK...")

    def _settings_widget_text_value(self, field: Dict[str, Any], widgets: Dict[str, Any]) -> str:
        field_type = str(field.get("type") or "")
        input_var = widgets.get("input_var")
        input_widget = widgets.get("input")

        if field_type == "bool" and input_var is not None:
            return "true" if bool(input_var.get()) else "false"
        if field_type == "enum" and input_var is not None and self._config_enum_options(field):
            return self._config_enum_option_to_value(str(input_var.get()))
        if input_var is not None:
            return str(input_var.get())
        if input_widget is not None and hasattr(input_widget, "get"):
            return str(input_widget.get())
        return ""

    def _send_config_reset_from_field(self, field_id: str):
        if not self.selected_device:
            self._append_log("Selecione um dispositivo para config/reset", tag="warn")
            return
        field = self._config_manifest_field(field_id)
        if not field:
            self._append_log(f"Campo de config indisponivel: {field_id}", tag="warn")
            return
        if self._config_has_flag(field, "read_only"):
            self._append_log(f"Campo read_only nao pode ser resetado: {field_id}", tag="warn")
            return

        self._send_cmd("config/reset", {"id": field_id})
        if hasattr(self, "settings_status_label"):
            self.settings_status_label.configure(text=f"config/reset enviado para {field_id}; aguardando ACK...")

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
        self._refresh_active_device_tab()
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
            ("config/get", lambda device_id=row_id: self._request_config_get_for_device(device_id), action_enabled),
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

    def _request_config_get_for_device(self, device_id: str):
        self._select_device_tab(device_id, "Settings")
        self.settings_manual_config_get_device_id = device_id
        self._send_cmd_to_device(device_id, "config/get")
        if hasattr(self, "settings_status_label"):
            self.settings_status_label.configure(text="config/get: aguardando resposta...")

    def _clear_retained_for_device_id(self, device_id: str):
        self.clear_retained_device_var.set(device_id)
        self._clear_retained_for_device()

    def _select_device_tab(self, device_id: str, tab_name: str):
        self.selected_device = device_id
        if self.tree.exists(device_id):
            self.tree.selection_set(device_id)
        self.clear_retained_device_var.set(device_id)
        self._update_settings_selection_status()
        if hasattr(self, "tabs"):
            self.tabs.set(tab_name)
        self._ensure_tab_panel(tab_name)
        self._refresh_device_details()
        self._refresh_active_device_tab()

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
        return self._current_tab_name() == "Status"

    def _current_tab_name(self) -> str:
        return self.tabs.get() if hasattr(self, "tabs") else ""

    def _on_main_tab_changed(self):
        current_tab = self._current_tab_name()
        self._ensure_tab_panel(current_tab)
        self._refresh_active_device_tab()

        if (
            self._is_status_tab_visible()
            and bool(self.technical_auto_update_var.get())
            and self.mqtt.connected
            and self.selected_device
            and not self._has_pending_command(self.selected_device, "get_technical_status")
        ):
            self._send_cmd_to_device(self.selected_device, "get_technical_status", log_publish=False)

    def _ensure_tab_panel(self, tab_name: str):
        if tab_name == "Status":
            self._ensure_status_panel()
        elif tab_name == "Comandos":
            self._ensure_commands_panel()
        elif tab_name == "Settings":
            self._ensure_settings_panel()

    def _refresh_active_device_tab(self):
        current_tab = self._current_tab_name()
        if current_tab == "Status":
            self._refresh_status_panel()
        elif current_tab == "Comandos":
            self._refresh_commands_panel()
        elif current_tab == "Settings":
            self._refresh_settings_panel()

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

    def _clear_retained_for_device(self):
        device_id = self.clear_retained_device_var.get().strip() or (self.selected_device or "")
        if not device_id:
            self._append_log("Informe ou selecione um device_id para limpar retained", tag="warn")
            return

        topics = [
            self._base_topic(device_id, "availability"),
            self._base_topic(device_id, "seen"),
            self._base_topic(device_id, "heartbeat"),
            self._base_topic(device_id, "state"),
            self._base_topic(device_id, "event"),
            self._base_topic(device_id, "meta", "config"),
            self._base_topic(device_id, "meta", "status"),
            self._base_topic(device_id, "meta", "commands"),
            self._base_topic(device_id, "cmd", "out"),
            self._base_topic(device_id, "cmd", "in"),
        ]
        ok_count, fail_count = self.mqtt.clear_retained_topics(topics)
        self._append_log(
            f"Limpeza retained para {device_id}: ok={ok_count} fail={fail_count}",
            tag="info" if fail_count == 0 else "warn",
        )

    def _extract_payload_timestamp(self, payload_obj: Optional[Dict[str, Any]]) -> Optional[datetime]:
        if not isinstance(payload_obj, dict):
            return None
        ts_value = payload_obj.get("last_seen_ts") or payload_obj.get("ts")
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

    def _request_config_get(self):
        self.settings_manual_config_get_device_id = self.selected_device
        self._send_cmd("config/get")
        if hasattr(self, "settings_status_label"):
            self.settings_status_label.configure(text="config/get: aguardando resposta...")

    def _apply_commands_get_result(self, commands_result: Dict[str, Any], device_id: Optional[str] = None):
        target_device_id = device_id or self.selected_device
        if target_device_id:
            device = self.devices.get(target_device_id)
            if device:
                topic = self._base_topic(target_device_id, "meta", "commands")
                device.last_messages["meta/commands"] = MessageSnapshot(
                    timestamp=datetime.now(),
                    topic=topic,
                    payload_obj=commands_result,
                    payload_raw=json.dumps(commands_result, ensure_ascii=True),
                )
        if self._current_tab_name() == "Comandos":
            self._refresh_commands_panel()

    def _apply_config_get_result(self, config_result: Dict[str, Any], device_id: Optional[str] = None):
        self.settings_loaded_device_id = device_id or self.selected_device
        force_reload = (
            self.settings_loaded_device_id is not None and
            self.settings_manual_config_get_device_id == self.settings_loaded_device_id
        )
        if force_reload:
            self.settings_manual_config_get_device_id = None
        if self.settings_loaded_device_id:
            device = self.devices.get(self.settings_loaded_device_id)
            if device:
                topic = self._base_topic(self.settings_loaded_device_id, "meta", "config")
                device.last_messages["meta/config"] = MessageSnapshot(
                    timestamp=datetime.now(),
                    topic=topic,
                    payload_obj=config_result,
                    payload_raw=json.dumps(config_result, ensure_ascii=True),
                )
        self._set_settings_raw_text(json.dumps(config_result, ensure_ascii=True, indent=2, sort_keys=True))
        if self._current_tab_name() == "Settings":
            self._refresh_settings_panel(force_reload=force_reload)
        elif force_reload:
            self.settings_force_sync_device_id = self.settings_loaded_device_id
        if hasattr(self, "settings_status_label"):
            self.settings_status_label.configure(
                text=(
                    f"config/get: {datetime.now().strftime('%Y-%m-%d %H:%M:%S')} | "
                    f"origem: {self.settings_loaded_device_id}"
                )
            )

    def _set_settings_raw_text(self, text: str):
        if self.settings_raw_text_value == text:
            return
        self.settings_raw_text_value = text
        if not hasattr(self, "settings_raw_text"):
            return
        self.settings_raw_text.configure(state="normal")
        self.settings_raw_text.delete("1.0", END)
        self.settings_raw_text.insert(END, text)
        self.settings_raw_text.configure(state="disabled")

    def _toggle_settings_raw_text(self):
        self.settings_raw_visible = not self.settings_raw_visible
        self._sync_settings_raw_visibility()

    def _sync_settings_raw_visibility(self):
        if not hasattr(self, "settings_raw_text"):
            return
        if self.settings_raw_visible:
            self.settings_raw_text.grid(row=3, column=0, sticky="nsew", padx=8, pady=(0, 8))
        else:
            self.settings_raw_text.grid_remove()
        if hasattr(self, "settings_raw_toggle_button"):
            text = "Ocultar JSON" if self.settings_raw_visible else "Mostrar JSON"
            if self.settings_raw_toggle_button.cget("text") != text:
                self.settings_raw_toggle_button.configure(text=text)

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
        except Exception as exc:
            self._append_log(f"Erro ao processar evento da UI: {exc}", tag="error")
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
        is_lwt_offline = (
            message_type == "availability"
            and isinstance(payload_obj, dict)
            and payload_obj.get("status") == "offline"
            and payload_obj.get("reason") == "lwt"
        )

        if is_empty_payload:
            device.last_messages.pop(message_type, None)
            if message_type in PRESENCE_MESSAGE_TYPES and not device.seen_live:
                device.last_seen = None
                device.online = False
            self._upsert_tree_row(device)
            self._append_log(f"Payload vazio recebido em {topic}; snapshot local removido", tag="info")
            if self.selected_device == device_id:
                self._refresh_device_details()
                self._refresh_active_device_tab()
            return

        if is_retained:
            # Retained snapshots nao significam dispositivo online agora.
            if counts_as_presence and payload_ts and not device.seen_live:
                if not device.last_seen or payload_ts > device.last_seen:
                    device.last_seen = payload_ts
            if is_lwt_offline:
                device.online = False
            if counts_as_presence and self.mqtt.connected and bool(self.auto_probe_var.get()) and not device.seen_live:
                now = datetime.now()
                if not device.last_probe_at or now - device.last_probe_at > timedelta(seconds=30):
                    device.last_probe_at = now
                    self._send_cmd_to_device(device_id, "get_state", log_publish=False)
        elif is_lwt_offline:
            device.online = False
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
        if is_lwt_offline:
            log_line = f"[lwt] {log_line}"
            log_tag = "mqtt_lwt"
        elif is_retained:
            log_line = f"[retained] {log_line}"
            log_tag = "mqtt_retained"
        elif message_type == "heartbeat":
            log_tag = "mqtt_heartbeat"
        elif message_type == "cmd/out" and self.selected_device == device_id:
            log_tag = "selected_cmd"
        else:
            log_tag = "mqtt_received"
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
            else:
                self._append_log(
                    f"cmd/out sem comando pendente correspondente: device={device_id} cmd_id={cmd_id or '-'}",
                    tag="warn",
                )

        if (
            message_type == "cmd/out"
            and self.selected_device == device_id
            and cmd_result_command == "config/get"
            and isinstance(payload_obj, dict)
            and payload_obj.get("ok") is True
            and isinstance(payload_obj.get("result"), dict)
        ):
            result = payload_obj.get("result")
            self._apply_config_get_result(result, device_id=device_id)

        if (
            message_type == "cmd/out"
            and self.selected_device == device_id
            and cmd_result_command == "commands/get"
            and isinstance(payload_obj, dict)
            and payload_obj.get("ok") is True
            and isinstance(payload_obj.get("result"), dict)
        ):
            result = payload_obj.get("result")
            self._apply_commands_get_result(result, device_id=device_id)

        if (
            message_type == "cmd/out"
            and self.selected_device == device_id
            and cmd_result_command in {"config/set", "config/reset"}
            and isinstance(payload_obj, dict)
            and hasattr(self, "settings_status_label")
        ):
            result = payload_obj.get("result")
            if payload_obj.get("ok") is True and isinstance(result, dict):
                self.settings_status_label.configure(
                    text=(
                        f"{cmd_result_command} OK: {result.get('id', '-')} | "
                        f"stored={result.get('stored', '-')} applied={result.get('applied', '-')} "
                        f"reboot={result.get('requires_reboot', '-')}"
                    )
                )
            elif payload_obj.get("ok") is False:
                self.settings_status_label.configure(
                    text=f"{cmd_result_command} falhou: {payload_obj.get('error', '-')}"
                )

        if self.selected_device == device_id:
            self._refresh_device_details()
            self._refresh_active_device_tab()

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
                self._refresh_active_device_tab()

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
        return (
            cmd_out.get("ok") is False
            or bool(cmd_out.get("error"))
            or bool(event.get("error"))
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
        availability = self._payload_for(device, "availability")
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
            state_text = self._field(availability, "status", "message", "state")
        if state_text != "-":
            parts.append(self._format_state(str(state_text)))

        last_error = (
            cmd_out.get("error")
            or event.get("error")
            or technical.get("last_error")
            or technical.get("error")
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
        availability = self._payload_for(device, "availability")
        seen = self._payload_for(device, "seen")
        config_manifest = self._payload_for(device, "meta/config")
        status_manifest = self._payload_for(device, "meta/status")
        commands_manifest = self._payload_for(device, "meta/commands")
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
        self._set_detail("availability", self._field(availability, "status", "message", "state"))

        self._set_detail("event", self._field(event, "event", "name", "type"))
        self._set_detail("event_message", self._field(event, "message", "detail", "error"))

        self._set_detail("cmd_ts", self._field(cmd_out, "ts"))
        self._set_detail("cmd_id", self._field(cmd_out, "cmd_id"))
        self._set_detail("cmd_ok", self._field(cmd_out, "ok"))
        self._set_detail("cmd_error", self._field(cmd_out, "error"))
        self._set_detail("cmd_result", self._field(cmd_out, "result"))
        self._set_detail("state_payload", self._compact_json(state))
        self._set_detail("availability_payload", self._compact_json(availability))
        self._set_detail("seen_payload", self._compact_json(seen))
        self._set_detail("heartbeat_payload", self._compact_json(heartbeat))
        self._set_detail("config_manifest_payload", self._compact_json(config_manifest))
        self._set_detail("status_manifest_payload", self._compact_json(status_manifest))
        self._set_detail("commands_manifest_payload", self._compact_json(commands_manifest))

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
            self._payload_for(device, "availability"),
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
        copy_button = ctk.CTkButton(
            frame,
            text="Copiar log",
            width=116,
            height=32,
            corner_radius=6,
            fg_color="transparent",
            hover_color=("gray84", "gray28"),
            text_color=("gray10", "gray90"),
            command=self._copy_log_from_context_menu,
        )
        copy_button.grid(row=0, column=0, padx=5, pady=(5, 2))
        clear_button = ctk.CTkButton(
            frame,
            text="Limpar log",
            width=116,
            height=32,
            corner_radius=6,
            fg_color="transparent",
            hover_color=("gray84", "gray28"),
            text_color=("gray10", "gray90"),
            command=self._clear_log_from_context_menu,
        )
        clear_button.grid(row=1, column=0, padx=5, pady=(2, 5))

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

    def _copy_log_from_context_menu(self):
        text = self.log_box.get("1.0", "end-1c")
        self.clipboard_clear()
        self.clipboard_append(text)
        self._hide_log_context_menu()
        self._append_log("Log copiado para a area de transferencia", tag="info")

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
        base_parts = self._base_topic().split("/")
        parts = topic.split("/")
        if len(parts) < len(base_parts) + 2:
            return None, None
        if parts[:len(base_parts)] != base_parts:
            return None, None

        device_id = parts[len(base_parts)]
        tail = parts[len(base_parts) + 1:]
        if tail == ["cmd", "out"]:
            return device_id, "cmd/out"
        if tail == ["meta", "config"]:
            return device_id, "meta/config"
        if tail == ["meta", "status"]:
            return device_id, "meta/status"
        if tail == ["meta", "commands"]:
            return device_id, "meta/commands"
        if len(tail) == 1 and tail[0] in {"availability", "seen", "heartbeat", "state", "event"}:
            return device_id, tail[0]
        return None, None

    def _base_topic(self, *parts: str) -> str:
        base = self.mqtt.base_topic if self.mqtt.connected else MQTTManager.normalize_base_topic(self.base_topic_var.get())
        return "/".join([base, *parts])


def main():
    app = App()

    def on_close():
        try:
            app.mqtt.disconnect(emit_event=False)
        except TclError:
            pass
        app.destroy()

    app.protocol("WM_DELETE_WINDOW", on_close)
    app.mainloop()


if __name__ == "__main__":
    main()
