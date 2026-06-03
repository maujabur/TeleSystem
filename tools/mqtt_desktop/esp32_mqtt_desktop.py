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


@dataclass
class MessageSnapshot:
    timestamp: datetime
    topic: str
    payload_obj: Optional[Dict[str, Any]]
    payload_raw: str


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
        self.pending_cmd_by_id: Dict[str, Tuple[str, str]] = {}
        self.mousewheel_scroll_frames: list[Any] = []
        self.tree_sort_column = "device_id"
        self.tree_sort_desc = False
        self.tree_heading_labels: Dict[str, str] = {}
        self.status_icons: Dict[str, tk.PhotoImage] = {}

        self.host_var = StringVar(value="localhost")
        self.port_var = StringVar(value="1883")
        self.user_var = StringVar(value="")
        self.pass_var = StringVar(value="")
        self.clear_retained_device_var = StringVar(value="")
        self.tls_var = ctk.BooleanVar(value=False)
        self.auto_probe_var = ctk.BooleanVar(value=True)
        self.heartbeat_timeout_var = StringVar(value="180")
        self.auto_connect_on_start_var = ctk.BooleanVar(value=False)
        self.technical_auto_update_var = ctk.BooleanVar(value=True)
        self.technical_update_interval_var = StringVar(value="3")
        self.conn_state_var = StringVar(value="Disconnected")

        self._build_ui()
        self._enable_mousewheel_scrolling()
        self._load_example_or_local_config()

        self.after(100, self._drain_events)
        self.after(1000, self._check_offline_devices)
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
        ctk.CTkButton(btns, text="Connect", command=self._connect).grid(row=0, column=0, sticky="ew", padx=(0, 4))
        ctk.CTkButton(btns, text="Disconnect", command=self._disconnect).grid(row=0, column=1, sticky="ew", padx=(4, 0))

        status_frame = ctk.CTkFrame(conn)
        status_frame.grid(row=8, column=0, columnspan=2, sticky="ew", padx=8, pady=(0, 8))
        self.conn_indicator = ctk.CTkLabel(status_frame, text=" ", width=18, height=18, fg_color="#a33", corner_radius=8)
        self.conn_indicator.grid(row=0, column=0, padx=(6, 8), pady=6)
        ctk.CTkLabel(status_frame, textvariable=self.conn_state_var).grid(row=0, column=1, sticky="w")

        devices_frame = ctk.CTkFrame(left)
        devices_frame.grid(row=2, column=0, sticky="nsew", padx=10, pady=(6, 10))
        devices_frame.grid_columnconfigure(0, weight=1)
        devices_frame.grid_rowconfigure(1, weight=1)

        ctk.CTkLabel(devices_frame, text="Dispositivos", font=ctk.CTkFont(size=16, weight="bold")).grid(
            row=0, column=0, sticky="w", padx=8, pady=(8, 4)
        )

        cols = ("device_id", "online", "last_seen", "fw", "session_id")
        self.tree = ttk.Treeview(devices_frame, columns=cols, show=("tree", "headings"), height=12)
        self.tree.grid(row=1, column=0, sticky="nsew", padx=8, pady=(0, 8))
        self.tree.bind("<<TreeviewSelect>>", self._on_select_device)
        self.tree.tag_configure("online", foreground="#1f8b24")
        self.tree.tag_configure("offline", foreground="#d11a2a")
        self.status_icons = {
            "online": self._create_status_icon("#25a83a", "#166b24"),
            "offline": self._create_status_icon("#d11a2a", "#85101b"),
        }
        self.tree.heading("#0", text="")
        self.tree.column("#0", width=28, minwidth=28, stretch=False, anchor="center")

        widths = {"device_id": 140, "online": 95, "last_seen": 170, "fw": 90, "session_id": 140}
        heading_labels = {
            "device_id": "device_id",
            "online": "online",
            "last_seen": "last_seen (local)",
            "fw": "fw",
            "session_id": "session_id",
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

        tabs = ctk.CTkTabview(top_container)
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
        self.log_menu = tk.Menu(self, tearoff=0)
        self.log_menu.add_command(label="Clear", command=self._clear_log)
        self.log_box.bind("<Button-3>", self._show_log_context_menu)
        self.log_box.bind("<Button-2>", self._show_log_context_menu)
        self.bind_all("<Button-1>", self._hide_log_context_menu, add="+")
        self.bind_all("<Escape>", self._hide_log_context_menu, add="+")

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
        self.settings_inputs = {}

        actions = ctk.CTkFrame(parent)
        actions.grid(row=0, column=0, sticky="ew", padx=8, pady=(8, 4))
        actions.grid_columnconfigure((0, 1, 2, 3), weight=1)
        ctk.CTkButton(actions, text="Ler settings", command=self._request_get_settings).grid(
            row=0, column=0, sticky="ew", padx=4, pady=4
        )
        self.btn_save_settings = ctk.CTkButton(actions, text="Salvar settings", command=self._send_set_settings)
        self.btn_save_settings.grid(
            row=0, column=1, sticky="ew", padx=4, pady=4
        )
        ctk.CTkButton(actions, text="Salvar heartbeat", command=self._send_set_heartbeat).grid(
            row=0, column=2, sticky="ew", padx=4, pady=4
        )
        ctk.CTkButton(actions, text="Apply + reboot", command=lambda: self._send_cmd("apply_and_reboot")).grid(
            row=0, column=3, sticky="ew", padx=4, pady=4
        )

        self.settings_status_label = ctk.CTkLabel(parent, text="Ultima leitura: - | leia os settings antes de salvar", anchor="w")
        self.settings_status_label.grid(row=1, column=0, sticky="ew", padx=8, pady=(0, 4))
        self.btn_save_settings.configure(state="disabled")

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
            ("bt_next", "BT_NEXT"),
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
        bt_text = (
            f"{'Habilitado' if bool(tech.get('bt_next_enabled')) else 'Desabilitado'} | "
            f"GPIO {self._value(tech, 'bt_next_gpio', '--')} | "
            f"{self._format_ms(self._num(tech, 'bt_next_pulse_ms'))}"
        )
        if tech.get("bt_next_last_error"):
            bt_text += f" | {tech.get('bt_next_last_error')}"

        counters_text = (
            f"envios {self._value(tech, 'acr_submitted_count', 0)} | "
            f"silencio {self._value(tech, 'acr_silence_discarded_count', 0)} | "
            f"erros ACR {self._value(tech, 'acr_error_count', 0)}"
        )
        vbat_text = self._format_vbat(vbat, get_state)
        power_good_text = self._format_power_good(power_good, vbat)
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
        self._set_status_label("card.bt_next.value", bt_text)
        self._set_status_label("card.bt_next.detail", " ")
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
        if bool(self.auto_connect_on_start_var.get()) and not self.mqtt.connected:
            self._append_log("Auto connect on start habilitado", tag="info")
            self._connect()

    def _connect(self):
        host, port, tls_enabled, error = self._resolve_broker_params(
            self.host_var.get().strip(),
            self.port_var.get().strip(),
            bool(self.tls_var.get()),
        )
        if error:
            self._append_log(error, tag="error")
            return

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
        self.mqtt.disconnect()

    def _on_select_device(self, _event=None):
        selection = self.tree.selection()
        if not selection:
            self.selected_device = None
            self._refresh_status_panel()
            return

        self.selected_device = selection[0]
        self.clear_retained_device_var.set(self.selected_device)
        self._refresh_device_details()
        self._refresh_status_panel()

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

    def _technical_status_auto_update_tick(self):
        interval_s = self._technical_update_interval_seconds()
        if (
            bool(self.technical_auto_update_var.get())
            and self.mqtt.connected
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

    def _has_pending_command(self, device_id: str, command: str) -> bool:
        return any(
            pending_device_id == device_id and pending_cmd == command
            for pending_device_id, pending_cmd in self.pending_cmd_by_id.values()
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

    def _send_cmd_to_device(
        self,
        device_id: str,
        command: str,
        extra_params: Optional[Dict[str, Any]] = None,
        log_publish: bool = True,
    ):
        if not device_id:
            self._append_log("Device_id invalido", tag="warn")
            return

        payload = {
            "name": command,
            "cmd_id": self._new_cmd_id(),
            "ts": datetime.now().isoformat(timespec="seconds"),
        }
        if extra_params:
            payload["args"] = extra_params

        ok, msg = self.mqtt.publish_command(device_id, payload)
        if not ok:
            self._append_log(msg, tag="error")
        elif log_publish:
            self._append_log(f"Cmd {command} enviado para {device_id}", tag="info")
        if ok:
            self.pending_cmd_by_id[payload["cmd_id"]] = (device_id, command)

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
        if not self.settings_loaded:
            self._append_log("Leia os settings primeiro para evitar envio invalido", tag="warn")
            return

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
            return

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

        for section_name, section_payload in changed_sections.items():
            self._send_cmd("set_settings", {section_name: section_payload})
        self._append_log(f"set_settings enviado em {len(changed_sections)} bloco(s)", tag="info")

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

    def _apply_settings_result(self, settings_result: Dict[str, Any]):
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

        self.settings_snapshot = json.loads(json.dumps(settings_result))
        self.settings_loaded = True
        if hasattr(self, "btn_save_settings"):
            self.btn_save_settings.configure(state="normal")

        if hasattr(self, "settings_status_label"):
            self.settings_status_label.configure(text=f"Ultima leitura: {datetime.now().strftime('%Y-%m-%d %H:%M:%S')}")

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
        self.conn_state_var.set(reason)
        self.conn_indicator.configure(fg_color="#1f8b24" if connected else "#a33")
        if connected:
            self._append_log("MQTT connected", tag="info")
            if bool(self.auto_probe_var.get()):
                self._probe_known_devices()
        else:
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

        payload_ts = self._extract_payload_timestamp(payload_obj)
        effective_ts = payload_ts or broker_time

        if is_retained:
            # Retained snapshots nao significam dispositivo online agora.
            if payload_ts:
                if not device.last_seen or payload_ts > device.last_seen:
                    device.last_seen = payload_ts
            if self.mqtt.connected and bool(self.auto_probe_var.get()) and not device.seen_live:
                now = datetime.now()
                if not device.last_probe_at or now - device.last_probe_at > timedelta(seconds=30):
                    device.last_probe_at = now
                    self._send_cmd_to_device(device_id, "get_state", log_publish=False)
        else:
            device.seen_live = True
            device.last_seen = effective_ts
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
                pending_device_id, pending_cmd = self.pending_cmd_by_id.pop(cmd_id)
                cmd_result_command = pending_cmd
                if pending_device_id == device_id and pending_cmd == "get_state":
                    result = payload_obj.get("result")
                    if isinstance(result, dict) and payload_obj.get("ok") is True:
                        device.last_get_state_result = result
                        device.last_get_state_at = effective_ts
                elif pending_device_id == device_id and pending_cmd == "get_technical_status":
                    result = payload_obj.get("result")
                    if isinstance(result, dict) and payload_obj.get("ok") is True:
                        device.last_technical_status_result = result
                        device.last_technical_status_at = effective_ts

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
                self._apply_settings_result(result)

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

        fw = payload_obj.get("fw") or payload_obj.get("firmware")
        if fw:
            device.fw = str(fw)

        session_id = payload_obj.get("session_id") or payload_obj.get("sid")
        if session_id:
            device.session_id = str(session_id)

    def _upsert_tree_row(self, device: DeviceInfo):
        last_seen_str = self._format_local_datetime(device.last_seen)
        status_value = "online" if device.online else "offline"
        status_display = status_value
        values = (
            device.device_id,
            status_display,
            last_seen_str,
            device.fw,
            device.session_id,
        )

        if self.tree.exists(device.device_id):
            self.tree.item(device.device_id, values=values, image=self.status_icons[status_value], tags=(status_value,))
        else:
            self.tree.insert("", END, iid=device.device_id, values=values, image=self.status_icons[status_value], tags=(status_value,))
        self._apply_tree_sort()

    def _on_tree_heading_click(self, col: str):
        if self.tree_sort_column == col:
            self.tree_sort_desc = not self.tree_sort_desc
        else:
            self.tree_sort_column = col
            self.tree_sort_desc = False
        self._refresh_tree_headings()
        self._apply_tree_sort()

    def _refresh_tree_headings(self):
        for col, label in self.tree_heading_labels.items():
            text = label
            if col == self.tree_sort_column:
                arrow = "↓" if self.tree_sort_desc else "↑"
                text = f"▶ {label} {arrow}"
            self.tree.heading(col, text=text, command=lambda c=col: self._on_tree_heading_click(c))

    def _apply_tree_sort(self):
        rows = list(self.tree.get_children(""))
        if not rows:
            return

        selected = self.tree.selection()
        selected_iid = selected[0] if selected else None

        rows.sort(key=self._tree_sort_key, reverse=self.tree_sort_desc)
        for idx, iid in enumerate(rows):
            self.tree.move(iid, "", idx)

        if selected_iid and self.tree.exists(selected_iid):
            self.tree.selection_set(selected_iid)

    def _tree_sort_key(self, iid: str):
        dev = self.devices.get(iid)
        if not dev:
            return ""

        col = self.tree_sort_column
        if col == "device_id":
            return dev.device_id.casefold()
        if col == "online":
            return 0 if dev.online else 1
        if col == "last_seen":
            return dev.last_seen or datetime.min
        if col == "fw":
            return (dev.fw or "").casefold()
        if col == "session_id":
            return (dev.session_id or "").casefold()
        return ""

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

        self._set_detail("device_id", device.device_id)
        self._set_detail("online", "online" if device.online else "offline")
        self._set_detail("last_seen", self._format_local_datetime(device.last_seen))
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

    def _show_log_context_menu(self, event):
        self.log_menu.post(event.x_root, event.y_root)
        return "break"

    def _hide_log_context_menu(self, _event=None):
        self.log_menu.unpost()

    def _append_log(self, text: str, tag: str = "info"):
        ts = datetime.now().strftime("%H:%M:%S")
        line = f"[{ts}] {text}\n"
        self.log_box.insert(END, line, tag)
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
