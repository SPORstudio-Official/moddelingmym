"""Living Notes — a private, local-first notebook for Windows and desktop Python."""
from __future__ import annotations

import json
import os
import sqlite3
import sys
import textwrap
import tkinter as tk
from datetime import datetime, timedelta
from pathlib import Path
from tkinter import filedialog, messagebox, ttk

APP_NAME = "Living Notes"
INK = "#182322"
MUTED = "#71807c"
PAPER = "#f5f6f0"
WHITE = "#ffffff"
GREEN = "#176b57"
PALE = "#e4eee8"
LINE = "#e4e9e3"
AMBER = "#e7a547"


def now() -> str:
    return datetime.now().isoformat(timespec="seconds")


def data_file() -> Path:
    if os.name == "nt":
        root = Path(os.environ.get("APPDATA", Path.home())) / "LivingNotes"
    else:
        root = Path.home() / ".living-notes"
    root.mkdir(parents=True, exist_ok=True)
    return root / "living-notes.db"


class Store:
    def __init__(self):
        self.path = data_file()
        self.db = sqlite3.connect(self.path)
        self.db.row_factory = sqlite3.Row
        self.db.execute("PRAGMA foreign_keys=ON")
        self.db.executescript("""
          CREATE TABLE IF NOT EXISTS notes (
            id INTEGER PRIMARY KEY, title TEXT NOT NULL, body TEXT NOT NULL DEFAULT '',
            kind TEXT NOT NULL DEFAULT 'Note', tags TEXT NOT NULL DEFAULT '',
            created_at TEXT NOT NULL, updated_at TEXT NOT NULL, viewed_at TEXT NOT NULL,
            reminder_days INTEGER NOT NULL DEFAULT 30, parent_id INTEGER,
            decision TEXT NOT NULL DEFAULT '', reasons TEXT NOT NULL DEFAULT '',
            alternatives TEXT NOT NULL DEFAULT '', expected TEXT NOT NULL DEFAULT '', actual TEXT NOT NULL DEFAULT '',
            characters TEXT NOT NULL DEFAULT '', world TEXT NOT NULL DEFAULT '', references_text TEXT NOT NULL DEFAULT '',
            FOREIGN KEY(parent_id) REFERENCES notes(id) ON DELETE SET NULL
          );
          CREATE TABLE IF NOT EXISTS links (source INTEGER NOT NULL, target INTEGER NOT NULL,
            label TEXT NOT NULL DEFAULT 'Related', created_at TEXT NOT NULL DEFAULT '', PRIMARY KEY(source,target),
            FOREIGN KEY(source) REFERENCES notes(id) ON DELETE CASCADE,
            FOREIGN KEY(target) REFERENCES notes(id) ON DELETE CASCADE);
          CREATE TABLE IF NOT EXISTS history (id INTEGER PRIMARY KEY, note_id INTEGER NOT NULL,
            title TEXT NOT NULL, body TEXT NOT NULL, saved_at TEXT NOT NULL, summary TEXT NOT NULL DEFAULT 'Edit',
            snapshot TEXT NOT NULL DEFAULT '{}',
            FOREIGN KEY(note_id) REFERENCES notes(id) ON DELETE CASCADE);
        """)
        history_columns = {row[1] for row in self.db.execute("PRAGMA table_info(history)")}
        if "snapshot" not in history_columns:
            self.db.execute("ALTER TABLE history ADD COLUMN snapshot TEXT NOT NULL DEFAULT '{}'")
        link_columns = {row[1] for row in self.db.execute("PRAGMA table_info(links)")}
        if "created_at" not in link_columns:
            self.db.execute("ALTER TABLE links ADD COLUMN created_at TEXT NOT NULL DEFAULT ''")
        self.db.commit()

    def all(self):
        return self.db.execute("SELECT * FROM notes ORDER BY updated_at DESC").fetchall()

    def get(self, note_id):
        return self.db.execute("SELECT * FROM notes WHERE id=?", (note_id,)).fetchone()

    def save(self, note_id, fields, summary="Edit"):
        stamp = now()
        if note_id:
            old = self.get(note_id)
            if old:
                snapshot = json.dumps({k: old[k] for k in old.keys() if k not in ("id", "created_at", "updated_at", "viewed_at", "parent_id")}, ensure_ascii=False)
                self.db.execute("INSERT INTO history(note_id,title,body,saved_at,summary,snapshot) VALUES(?,?,?,?,?,?)",
                                (note_id, old['title'], old['body'], stamp, summary, snapshot))
                cols = ",".join(f"{k}=?" for k in fields)
                self.db.execute(f"UPDATE notes SET {cols}, updated_at=? WHERE id=?", (*fields.values(), stamp, note_id))
        else:
            keys = list(fields)
            self.db.execute(f"INSERT INTO notes({','.join(keys)},created_at,updated_at,viewed_at) VALUES({','.join('?' for _ in keys)},?,?,?)",
                            (*fields.values(), stamp, stamp, stamp))
            note_id = self.db.execute("SELECT last_insert_rowid()").fetchone()[0]
        self.db.commit()
        return note_id

    def viewed(self, note_id):
        self.db.execute("UPDATE notes SET viewed_at=? WHERE id=?", (now(), note_id)); self.db.commit()

    def links(self, note_id):
        return self.db.execute("SELECT n.id,n.title,l.label FROM links l JOIN notes n ON n.id=l.target WHERE l.source=? UNION SELECT n.id,n.title,l.label FROM links l JOIN notes n ON n.id=l.source WHERE l.target=?", (note_id, note_id)).fetchall()

    def add_link(self, a, b):
        if a and b and a != b:
            self.db.execute("INSERT OR IGNORE INTO links(source,target,created_at) VALUES(?,?,?)", (a,b,now())); self.db.commit()

    def history(self, note_id):
        return self.db.execute("SELECT * FROM history WHERE note_id=? ORDER BY saved_at DESC", (note_id,)).fetchall()

    def close(self):
        self.db.close()


class LivingNotes(tk.Tk):
    def __init__(self):
        super().__init__()
        self.store = Store()
        self.title("Living Notes")
        self.geometry("1260x820")
        self.minsize(1000, 650)
        self.configure(bg=PAPER)
        self.selected = None
        self.active_filter = "All notes"
        self.query = tk.StringVar()
        self.status = tk.StringVar(value="Your ideas, connected and growing.")
        self._style()
        self._layout()
        self.refresh_list()
        self.protocol("WM_DELETE_WINDOW", self.on_close)

    def _style(self):
        s = ttk.Style(self)
        try: s.theme_use("clam")
        except tk.TclError: pass
        s.configure("Treeview", background=WHITE, fieldbackground=WHITE, foreground=INK,
                    rowheight=42, borderwidth=0, font=("Segoe UI", 10))
        s.configure("Treeview.Heading", background=PAPER, foreground=MUTED, relief="flat",
                    font=("Segoe UI", 9, "bold"))
        s.map("Treeview", background=[("selected", PALE)], foreground=[("selected", INK)])
        s.configure("TCombobox", padding=7)
        s.configure("TButton", font=("Segoe UI", 9), padding=(10, 7))
        s.configure("Accent.TButton", background=GREEN, foreground=WHITE, font=("Segoe UI", 9, "bold"), padding=(13, 9))
        s.map("Accent.TButton", background=[("active", "#115744")])

    def _layout(self):
        top = tk.Frame(self, bg=WHITE, height=66); top.pack(fill="x")
        tk.Label(top, text="✳", bg=WHITE, fg=GREEN, font=("Segoe UI", 24, "bold")).pack(side="left", padx=(24, 10))
        tk.Label(top, text="Living Notes", bg=WHITE, fg=INK, font=("Segoe UI", 17, "bold")).pack(side="left")
        tk.Label(top, text="A notebook that remembers with you", bg=WHITE, fg=MUTED, font=("Segoe UI", 10)).pack(side="left", padx=16)
        ttk.Button(top, text="Export everything ↗", command=self.export).pack(side="right", padx=(8, 24), pady=14)
        ttk.Button(top, text="＋  New note", style="Accent.TButton", command=self.new_note).pack(side="right", pady=14)

        shell = tk.Frame(self, bg=PAPER); shell.pack(fill="both", expand=True)
        rail = tk.Frame(shell, bg="#eef1ea", width=205); rail.pack(side="left", fill="y"); rail.pack_propagate(False)
        tk.Label(rail, text="YOUR SPACE", bg="#eef1ea", fg=MUTED, font=("Segoe UI", 8, "bold")).pack(anchor="w", padx=21, pady=(25, 10))
        for name, icon in [("All notes", "▤"), ("Decisions", "◇"), ("Creative", "✧"), ("Rediscover", "↻"), ("Knowledge map", "⌘"), ("Timeline", "◷")]:
            b = tk.Button(rail, text=f"{icon}   {name}", anchor="w", bd=0, bg="#eef1ea", fg=INK,
                          activebackground=PALE, font=("Segoe UI", 10), padx=18, pady=11,
                          command=lambda n=name: self.navigate(n))
            b.pack(fill="x", padx=8)
        tk.Frame(rail, bg="#dfe5dc", height=1).pack(fill="x", padx=18, pady=18)
        tk.Label(rail, text="LOCAL & PRIVATE", bg="#eef1ea", fg=GREEN, font=("Segoe UI", 8, "bold")).pack(anchor="w", padx=20)
        tk.Label(rail, text="Your notes stay on this device.\nNo account or cloud required.", justify="left", bg="#eef1ea", fg=MUTED, font=("Segoe UI", 9), wraplength=165).pack(anchor="w", padx=20, pady=8)

        self.main = tk.Frame(shell, bg=PAPER); self.main.pack(side="left", fill="both", expand=True, padx=26, pady=24)
        self.heading = tk.Label(self.main, text="Your thinking, still alive.", bg=PAPER, fg=INK, font=("Segoe UI", 22, "bold")); self.heading.pack(anchor="w")
        self.subheading = tk.Label(self.main, text="Capture a thought. Let it find its way back to you.", bg=PAPER, fg=MUTED, font=("Segoe UI", 10)); self.subheading.pack(anchor="w", pady=(4, 19))
        self.content = tk.Frame(self.main, bg=PAPER); self.content.pack(fill="both", expand=True)
        self._show_notes()
        footer = tk.Label(self, textvariable=self.status, anchor="w", bg=WHITE, fg=MUTED, padx=20, pady=8, font=("Segoe UI", 9)); footer.pack(fill="x", side="bottom")

    def clear_content(self):
        for child in self.content.winfo_children(): child.destroy()

    def navigate(self, name):
        self.active_filter = name
        labels = {"All notes": ("Your thinking, still alive.", "Capture a thought. Let it find its way back to you."),
                  "Decisions": ("Decisions, with their why.", "Keep the reasoning and the outcome together."),
                  "Creative": ("A home for the unfinished.", "Characters, worlds, fragments, and what might happen next."),
                  "Rediscover": ("A few thoughts from the past.", "Notes you have not revisited in a while, ready when you are."),
                  "Knowledge map": ("See how ideas connect.", "Relationships are lightweight; your notes stay yours."),
                  "Timeline": ("Your knowledge, over time.", "Created, changed, and connected — in one living stream.")}
        self.heading.config(text=labels[name][0]); self.subheading.config(text=labels[name][1])
        if name == "Knowledge map": self._show_map()
        elif name == "Timeline": self._show_timeline()
        else: self._show_notes()

    def _show_notes(self):
        self.clear_content()
        bar = tk.Frame(self.content, bg=PAPER); bar.pack(fill="x", pady=(0, 12))
        search = tk.Entry(bar, textvariable=self.query, bg=WHITE, fg=INK, relief="flat", font=("Segoe UI", 11), insertbackground=GREEN)
        search.pack(side="left", fill="x", expand=True, ipady=11, padx=(0, 10)); search.insert(0, "")
        search.bind("<KeyRelease>", lambda e: self.refresh_list())
        tk.Label(bar, text="⌕", bg=WHITE, fg=MUTED, font=("Segoe UI", 16)).place(in_=search, relx=0.98, rely=.5, anchor="e")
        ttk.Button(bar, text="＋ Connect", command=self.connect_notes).pack(side="right")
        panel = tk.Frame(self.content, bg=WHITE, highlightbackground=LINE, highlightthickness=1); panel.pack(fill="both", expand=True)
        self.tree = ttk.Treeview(panel, columns=("kind", "tags", "updated"), show="tree headings", selectmode="browse")
        self.tree.heading("#0", text="NOTE"); self.tree.heading("kind", text="TYPE"); self.tree.heading("tags", text="TOPICS"); self.tree.heading("updated", text="UPDATED")
        self.tree.column("#0", width=340, minwidth=220); self.tree.column("kind", width=105, anchor="w"); self.tree.column("tags", width=240); self.tree.column("updated", width=130, anchor="w")
        scroll = ttk.Scrollbar(panel, orient="vertical", command=self.tree.yview); self.tree.configure(yscrollcommand=scroll.set)
        self.tree.pack(side="left", fill="both", expand=True, padx=8, pady=8); scroll.pack(side="right", fill="y", pady=8)
        self.tree.bind("<<TreeviewSelect>>", self.pick_note)
        self.refresh_list()

    def refresh_list(self):
        if not hasattr(self, "tree") or not self.tree.winfo_exists(): return
        self.tree.delete(*self.tree.get_children())
        q = self.query.get().strip().lower()
        for n in self.store.all():
            if self.active_filter == "Decisions" and n["kind"] != "Decision": continue
            if self.active_filter == "Creative" and n["kind"] != "Creative": continue
            if self.active_filter == "Rediscover":
                try: due = datetime.fromisoformat(n["viewed_at"]) < datetime.now() - timedelta(days=int(n["reminder_days"]))
                except Exception: due = False
                if not due: continue
            searchable = " ".join(str(n[k]) for k in n.keys()).lower()
            if q and q not in searchable: continue
            title = n["title"] or "Untitled thought"
            updated = n["updated_at"][:10]
            self.tree.insert("", "end", iid=str(n["id"]), text=title, values=(n["kind"], n["tags"], updated))

    def pick_note(self, _event=None):
        item = self.tree.selection()
        if item: self.open_note(int(item[0]))

    def open_note(self, note_id):
        note = self.store.get(note_id)
        if not note: return
        self.selected = note_id; self.store.viewed(note_id)
        self.active_filter = self.active_filter
        if not hasattr(self, "editor") or not self.editor.winfo_exists(): self._show_editor()
        self._load_note(note)

    def new_note(self):
        self.active_filter = "All notes"
        self.heading.config(text="A new thought begins."); self.subheading.config(text="Write naturally. Structure can come later.")
        self.clear_content(); self.selected = None; self._show_editor()
        self._load_note({"title":"", "body":"", "kind":"Note", "tags":"", "reminder_days":30,
                         "decision":"", "reasons":"", "alternatives":"", "expected":"", "actual":"",
                         "characters":"", "world":"", "references_text":""})

    def _show_editor(self):
        self.clear_content()
        self.editor = tk.Frame(self.content, bg=WHITE, highlightbackground=LINE, highlightthickness=1); self.editor.pack(fill="both", expand=True)
        top = tk.Frame(self.editor, bg=WHITE); top.pack(fill="x", padx=22, pady=(17, 4))
        self.title_entry = tk.Entry(top, bd=0, bg=WHITE, fg=INK, font=("Segoe UI", 21, "bold")); self.title_entry.pack(side="left", fill="x", expand=True, ipady=8)
        self.kind_var = tk.StringVar(value="Note")
        ttk.Combobox(top, textvariable=self.kind_var, values=("Note", "Decision", "Creative", "Project", "Research"), state="readonly", width=12).pack(side="right", padx=(12, 0))
        ttk.Button(top, text="← Notes", command=self.back_to_notes).pack(side="right")
        meta = tk.Frame(self.editor, bg=WHITE); meta.pack(fill="x", padx=22, pady=6)
        tk.Label(meta, text="TOPICS", bg=WHITE, fg=MUTED, font=("Segoe UI", 8, "bold")).pack(side="left")
        self.tags_entry = tk.Entry(meta, bd=0, bg=WHITE, fg=GREEN, font=("Segoe UI", 10)); self.tags_entry.pack(side="left", fill="x", expand=True, padx=10, ipady=5)
        self.body = tk.Text(self.editor, wrap="word", bd=0, bg=WHITE, fg=INK, font=("Segoe UI", 11), padx=22, pady=14, undo=True, spacing1=3, spacing3=6)
        self.body.pack(fill="both", expand=True)
        sep = tk.Frame(self.editor, bg=LINE, height=1); sep.pack(fill="x", padx=22)
        lower = tk.Frame(self.editor, bg=WHITE); lower.pack(fill="x", padx=22, pady=12)
        self.details = tk.Frame(self.editor, bg="#f8f9f5"); self.details.pack(fill="x", padx=22, pady=(0, 12))
        self.detail_fields = {}; self.detail_rows = {}
        for key, label in [("decision","DECISION"),("reasons","WHY THIS WAY"),("alternatives","ALTERNATIVES"),("expected","EXPECTED OUTCOME"),("actual","ACTUAL RESULT"),("characters","CHARACTERS"),("world","WORLD / TIMELINE"),("references_text","REFERENCES / RESEARCH")]:
            row = tk.Frame(self.details, bg="#f8f9f5")
            tk.Label(row, text=label, width=19, anchor="w", bg="#f8f9f5", fg=MUTED, font=("Segoe UI", 8, "bold")).pack(side="left")
            ent = tk.Entry(row, bd=0, bg="#f8f9f5", fg=INK, font=("Segoe UI", 9)); ent.pack(side="left", fill="x", expand=True, ipady=4)
            self.detail_fields[key] = ent; self.detail_rows[key] = row
        tk.Label(lower, text="Resurface after", bg=WHITE, fg=MUTED, font=("Segoe UI", 9)).pack(side="left")
        self.reminder_var = tk.StringVar(value="30")
        ttk.Combobox(lower, textvariable=self.reminder_var, values=("7", "30", "60", "90", "180", "365"), width=5).pack(side="left", padx=7)
        tk.Label(lower, text="days away", bg=WHITE, fg=MUTED, font=("Segoe UI", 9)).pack(side="left")
        ttk.Button(lower, text="Connect", command=self.connect_notes).pack(side="right", padx=3)
        ttk.Button(lower, text="History", command=self.show_history).pack(side="right", padx=3)
        ttk.Button(lower, text="Branch", command=self.branch).pack(side="right", padx=3)
        ttk.Button(lower, text="Merge…", command=self.merge_note).pack(side="right", padx=3)
        ttk.Button(lower, text="Save note", style="Accent.TButton", command=self.save_note).pack(side="right", padx=3)
        self._render_kind_fields()
        self.kind_var.trace_add("write", lambda *_: self._render_kind_fields())

    def _render_kind_fields(self):
        kind = self.kind_var.get()
        keys = (["decision", "reasons", "alternatives", "expected", "actual"] if kind == "Decision" else
                ["characters", "world", "references_text"] if kind == "Creative" else [])
        for key, row in self.detail_rows.items():
            row.pack_forget()
            if key in keys: row.pack(fill="x", padx=10, pady=3)
        self.details.pack_forget()
        if keys: self.details.pack(fill="x", padx=22, pady=(0,12), after=self.body)

    def _load_note(self, n):
        self.title_entry.delete(0, "end"); self.title_entry.insert(0, n["title"])
        self.kind_var.set(n["kind"]); self.tags_entry.delete(0, "end"); self.tags_entry.insert(0, n["tags"])
        self.body.delete("1.0", "end"); self.body.insert("1.0", n["body"])
        self.reminder_var.set(str(n["reminder_days"]))
        for key, ent in self.detail_fields.items():
            ent.delete(0, "end"); ent.insert(0, n[key])

    def _fields(self):
        fields = {"title":self.title_entry.get().strip() or "Untitled thought", "body":self.body.get("1.0", "end-1c"),
                  "kind":self.kind_var.get(), "tags":self.tags_entry.get().strip(), "reminder_days":int(self.reminder_var.get() or 30)}
        fields.update({k:e.get().strip() for k,e in self.detail_fields.items()})
        return fields

    def save_note(self, summary="Edit"):
        fields = self._fields()
        was_new = self.selected is None
        self.selected = self.store.save(self.selected, fields, "Created" if was_new else summary)
        self.status.set(f"Saved locally · {self.store.path}")
        self.refresh_list()
        if hasattr(self, "tree") and self.tree.winfo_exists() and str(self.selected) in self.tree.get_children():
            self.tree.selection_set(str(self.selected))

    def back_to_notes(self):
        if self.title_entry.get().strip() or self.body.get("1.0", "end-1c").strip():
            self.save_note()
        self.navigate("All notes")

    def branch(self):
        if not self.selected: self.save_note()
        source = self.store.get(self.selected)
        if source:
            fields = {k:source[k] for k in source.keys() if k in ("title","body","kind","tags","reminder_days","decision","reasons","alternatives","expected","actual","characters","world","references_text")}
            fields["title"] = source["title"] + " — alternate"
            new_id = self.store.save(None, fields)
            self.store.db.execute("UPDATE notes SET parent_id=? WHERE id=?", (self.selected,new_id)); self.store.db.commit()
            self.status.set("Created a branch. The original note remains unchanged.")
            self.refresh_list(); self.open_note(new_id)

    def merge_note(self):
        if not self.selected: self.save_note()
        candidates = [n for n in self.store.all() if n["id"] != self.selected]
        if not candidates: messagebox.showinfo("Merge", "Create another note first, then combine it here."); return
        chosen = self._choose_note("Choose a note to merge into this one", candidates)
        if chosen:
            n = self.store.get(chosen)
            if messagebox.askyesno("Keep both originals", f"Append “{n['title']}” to this note? The source note will stay untouched."):
                self.body.insert("end", f"\n\n--- Merged from: {n['title']} ---\n{n['body']}")
                self.store.add_link(self.selected, chosen)
                self.save_note("Merged content")
                self.status.set("Merged content; both original notes remain available.")

    def _choose_note(self, title, notes):
        win = tk.Toplevel(self); win.title(title); win.geometry("450x400"); win.transient(self); win.grab_set()
        tk.Label(win,text=title,font=("Segoe UI",12,"bold")).pack(anchor="w",padx=16,pady=12)
        box=tk.Listbox(win,font=("Segoe UI",10),bd=0,selectbackground=PALE,activestyle="none")
        for n in notes: box.insert("end",f"{n['title']}   ·   {n['kind']}")
        box.pack(fill="both",expand=True,padx=16,pady=8)
        out=[]
        def done():
            if box.curselection(): out.append(notes[box.curselection()[0]]["id"])
            win.destroy()
        ttk.Button(win,text="Choose",command=done).pack(anchor="e",padx=16,pady=12)
        self.wait_window(win)
        return out[0] if out else None

    def connect_notes(self):
        if not self.selected:
            messagebox.showinfo("Connect notes", "Open a note first, then select Connect to relate it to another idea."); return
        options = [n for n in self.store.all() if n["id"] != self.selected and all(x["id"] != n["id"] for x in self.store.links(self.selected))]
        chosen = self._choose_note("Connect this thought to…", options) if options else None
        if chosen:
            self.store.add_link(self.selected, chosen); self.status.set("Relationship added to your knowledge map.")

    def show_history(self):
        if not self.selected: messagebox.showinfo("Note history", "Save the note once to begin its version history."); return
        history = self.store.history(self.selected)
        win = tk.Toplevel(self); win.title("Note history"); win.geometry("760x520"); win.configure(bg=PAPER)
        tk.Label(win,text="Previous versions",bg=PAPER,fg=INK,font=("Segoe UI",17,"bold")).pack(anchor="w",padx=20,pady=(18,8))
        if not history: tk.Label(win,text="Your next save will appear here.",bg=PAPER,fg=MUTED).pack(anchor="w",padx=20)
        for h in history:
            row=tk.Frame(win,bg=WHITE);row.pack(fill="x",padx=20,pady=5)
            tk.Label(row,text=f"{h['saved_at'][:16]}  ·  {h['summary']}  ·  {h['title']}",bg=WHITE,fg=INK,font=("Segoe UI",10,"bold")).pack(anchor="w",padx=12,pady=(10,4))
            tk.Label(row,text=textwrap.shorten(h['body'].replace("\n"," "),width=100),bg=WHITE,fg=MUTED,anchor="w").pack(fill="x",padx=12,pady=(0,8))
            ttk.Button(row,text="Restore this version",command=lambda item=h:self.restore(item,win)).pack(anchor="e",padx=10,pady=(0,8))

    def restore(self, h, win):
        try: snapshot = json.loads(h["snapshot"] or "{}")
        except (TypeError, json.JSONDecodeError): snapshot = {}
        self.title_entry.delete(0,"end");self.title_entry.insert(0,snapshot.get("title",h["title"]))
        self.body.delete("1.0","end");self.body.insert("1.0",snapshot.get("body",h["body"]))
        self.kind_var.set(snapshot.get("kind",self.kind_var.get()))
        self.tags_entry.delete(0,"end");self.tags_entry.insert(0,snapshot.get("tags",""))
        self.reminder_var.set(str(snapshot.get("reminder_days",30)))
        for key, ent in self.detail_fields.items():
            ent.delete(0,"end");ent.insert(0,snapshot.get(key,""))
        self.save_note("Restored previous version");win.destroy()

    def _show_map(self):
        self.clear_content()
        notes=self.store.all()
        canvas=tk.Canvas(self.content,bg=WHITE,highlightthickness=1,highlightbackground=LINE)
        canvas.pack(fill="both",expand=True)
        if not notes:
            canvas.create_text(300,150,text="Your map will grow as you write and connect notes.",fill=MUTED,font=("Segoe UI",13));return
        nodes=notes[:24]; cx,cy=430,280; radius=min(210,90+len(nodes)*6)
        coords={}
        for i,n in enumerate(nodes):
            import math
            angle=2*math.pi*i/max(1,len(nodes))
            x=cx+radius*math.cos(angle);y=cy+radius*.7*math.sin(angle);coords[n["id"]]=(x,y)
        ids={n["id"] for n in nodes}
        for n in nodes:
            for link in self.store.links(n["id"]):
                if link["id"] in ids and n["id"] < link["id"]:
                    x1,y1=coords[n["id"]];x2,y2=coords[link["id"]];canvas.create_line(x1,y1,x2,y2,fill="#b9c9bd",width=2)
        for n in nodes:
            x,y=coords[n["id"]]; color=GREEN if n["kind"]=="Decision" else ("#b77932" if n["kind"]=="Creative" else "#558978")
            canvas.create_oval(x-52,y-23,x+52,y+23,fill=color,outline="")
            canvas.create_text(x,y,text=textwrap.shorten(n["title"],width=16),fill=WHITE,font=("Segoe UI",9,"bold"))
            canvas.tag_bind(canvas.find_all()[-1],"<Button-1>",lambda e,i=n["id"]:self.open_note(i))
        tk.Label(self.content,text="Hint: select a note and use ＋ Connect to grow the map.",bg=PAPER,fg=MUTED,font=("Segoe UI",9)).pack(anchor="w",pady=8)

    def _show_timeline(self):
        self.clear_content()
        frame=tk.Frame(self.content,bg=WHITE,highlightbackground=LINE,highlightthickness=1);frame.pack(fill="both",expand=True)
        canv=tk.Canvas(frame,bg=WHITE,highlightthickness=0);scroll=ttk.Scrollbar(frame,orient="vertical",command=canv.yview);inner=tk.Frame(canv,bg=WHITE)
        inner.bind("<Configure>",lambda e:canv.configure(scrollregion=canv.bbox("all")))
        canv.create_window((0,0),window=inner,anchor="nw",width=780);canv.configure(yscrollcommand=scroll.set)
        canv.pack(side="left",fill="both",expand=True);scroll.pack(side="right",fill="y")
        events=[]
        for n in self.store.all():
            events.append((n["created_at"],"Created",n["title"],n["id"]))
            if n["updated_at"] != n["created_at"]: events.append((n["updated_at"],"Changed",n["title"],n["id"]))
            if n["parent_id"]: events.append((n["created_at"],"Branched from another idea",n["title"],n["id"]))
            for link in self.store.links(n["id"]):
                if link["id"] > n["id"]:
                    record = self.store.db.execute("SELECT created_at FROM links WHERE source=? AND target=? UNION SELECT created_at FROM links WHERE source=? AND target=? LIMIT 1", (n["id"],link["id"],link["id"],n["id"])).fetchone()
                    if record and record["created_at"]:
                        events.append((record["created_at"],"Connected to another note",n["title"],n["id"]))
        for stamp,kind,title,nid in sorted(events,reverse=True):
            row=tk.Frame(inner,bg=WHITE);row.pack(fill="x",padx=22,pady=9)
            tk.Label(row,text="●",fg=GREEN,bg=WHITE,font=("Segoe UI",11)).pack(side="left",padx=(0,12))
            tk.Label(row,text=f"{stamp[:16]}   {kind}",fg=MUTED,bg=WHITE,font=("Segoe UI",9)).pack(side="left")
            b=tk.Button(row,text=title,bd=0,bg=WHITE,fg=INK,font=("Segoe UI",10,"bold"),command=lambda i=nid:self.open_note(i));b.pack(side="left",padx=12)
        if not events: tk.Label(inner,text="Your note timeline will appear here.",bg=WHITE,fg=MUTED).pack(pady=30)

    def export(self):
        target=filedialog.asksaveasfilename(title="Export all notes",defaultextension=".json",filetypes=[("JSON backup","*.json")],initialfile="living-notes-backup.json")
        if not target:return
        result=[]
        for n in self.store.all():
            item=dict(n); item["relationships"]=[dict(x) for x in self.store.links(n["id"])];item["history"]=[dict(x) for x in self.store.history(n["id"])];result.append(item)
        with open(target,"w",encoding="utf-8") as f:json.dump({"app":APP_NAME,"exported_at":now(),"notes":result},f,ensure_ascii=False,indent=2)
        self.status.set(f"Exported {len(result)} notes · {target}")
        messagebox.showinfo("Export complete",f"Exported {len(result)} notes, links, and version history.")

    def on_close(self):
        self.store.close();self.destroy()


if __name__ == "__main__":
    LivingNotes().mainloop()
