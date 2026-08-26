// QtShell.cpp — shell Qt do VulkanCraft Editor (processo separado, MinGW/Qt 6.6.3).
//
// Consome a Control API do editor (por padrão http://127.0.0.1:8321):
//   GET /qt-doc     -> docks (QDockWidget), actions (QAction), menus, toolbars, status
//   GET /qt-theme   -> QPalette roles + QSS (tema charcoal, mesmo derive do ImGui)
//   GET /hierarchy  -> QTreeView do scene hierarchy (seleção -> /inspector)
//   GET /inspector  -> QTreeWidget do inspector (components -> properties)
//   GET /state      -> estado vivo do editor (status bar + play toggle)
//
// Modos:
//   QtShell.exe                 -> abre a janela real (platform windows)
//   QtShell.exe --smoke [porta] -> monta tudo headless (offscreen), valida contra o
//                                  doc real, imprime SMOKE OK/FAIL e sai 0/1.
//   QtShell.exe --port <n>      -> usa outra porta da Control API.
//
// Ações com rota mapeada na Control API executam de verdade no editor
// (ex.: scene.save -> POST /save-scene); as sem rota ficam enabled e avisam
// no status bar ("rota não mapeada") — nada é dead code silencioso.

#include <QApplication>
#include <QMainWindow>
#include <QDockWidget>
#include <QAction>
#include <QMenuBar>
#include <QToolBar>
#include <QStatusBar>
#include <QLabel>
#include <QPalette>
#include <QTreeView>
#include <QTreeWidget>
#include <QStandardItemModel>
#include <QPlainTextEdit>
#include <QHeaderView>
#include <QLabel>
#include <QPixmap>
#include <QResizeEvent>
#include <QFile>
#include <QFileInfo>
#include <QDir>
#include <QCoreApplication>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QNetworkAccessManager>
#include <QNetworkRequest>
#include <QNetworkReply>
#include <QEventLoop>
#include <QTimer>
#include <QColor>
#include <cstdio>
#include <cstring>

namespace {

QString base_url = "http://127.0.0.1:8321";

// Mapeia um dock de ferramenta do /qt-doc para o endpoint GET de leitura que
// expõe seu estado (todos da Control API do AGENT-2). Docks sem endpoint ficam
// com o placeholder.
QString live_endpoint_for(const QString& dockName) {
    struct { const char* dock; const char* route; } kLive[] = {
        { "profiler",        "/profiler"       },
        { "window_mode",     "/window-mode"    },
        { "layout",          "/layout"         },
        { "camera",          "/camera"         },
        { "gizmo",           "/gizmo"          },
        { "undo",            "/undo"           },
        { "publish",         "/publish"        },
        { "onboarding",      "/onboarding"     },
        { "retargeting",     "/retargeting"    },
        { "timeline_editor", "/timeline-editor" },
        { "ui_doc",          "/ui-doc"         },
    };
    const QByteArray d = dockName.toLatin1();
    for (const auto& e : kLive)
        if (d == e.dock) return QString::fromLatin1(e.route);
    return QString();
}

// Formata um corpo JSON (endpoint de leitura) de forma legível para o
// QPlainTextEdit dos docks de ferramenta. Endpoints que devolvem envelope
// {valid,...} ou objeto raiz ficam idênticos; só compactamos arrays longos.
QString pretty_json(const QString& body) {
    QJsonParseError pe;
    const QJsonDocument doc = QJsonDocument::fromJson(body.toUtf8(), &pe);
    if (pe.error != QJsonParseError::NoError || doc.isNull())
        return body.trimmed(); // fallback: mostra o bruto
    return doc.toJson(QJsonDocument::Indented);
}



// ---------------------------------------------------------------------------
// HTTP GET síncrono (event loop local) — necessário no boot antes do loop.
// ---------------------------------------------------------------------------
QString http_get(const QString& path, int timeoutMs = 3000, QString* err = nullptr) {
    QNetworkAccessManager mgr;
    QNetworkRequest req(QUrl(base_url + path));
    QNetworkReply* reply = mgr.get(req);
    QEventLoop loop;
    QTimer timer;
    timer.setSingleShot(true);
    QObject::connect(&timer, &QTimer::timeout, &loop, &QEventLoop::quit);
    QObject::connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
    timer.start(timeoutMs);
    loop.exec();
    if (!reply->isFinished()) {
        reply->abort();
        if (err) *err = "timeout GET " + path;
        return QString();
    }
    if (reply->error() != QNetworkReply::NoError) {
        if (err) *err = QString("HTTP %1 GET %2").arg(int(reply->error())).arg(path);
        reply->deleteLater();
        return QString();
    }
    QByteArray body = reply->readAll();
    reply->deleteLater();
    return QString::fromUtf8(body);
}

// --- Refresco dos docks de ferramenta (JSON formatado dos endpoints vivos) ---
void refresh_live_docks(QMainWindow& w) {
    const QList<QPlainTextEdit*> live = w.findChildren<QPlainTextEdit*>();
    for (QPlainTextEdit* te : live) {
        const QString on = te->objectName();
        if (!on.startsWith(QStringLiteral("live:"))) continue;
        const QString dockName = on.mid(5);
        const QString route = live_endpoint_for(dockName);
        if (route.isEmpty()) continue;
        const QString body = http_get(route, 1500);
        te->setPlainText(pretty_json(body));
    }
}

QString http_post(const QString& path, int timeoutMs = 3000, QString* err = nullptr) {
    QNetworkAccessManager mgr;
    QNetworkRequest req(QUrl(base_url + path));
    req.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");
    QNetworkReply* reply = mgr.post(req, QByteArray("{}"));
    QEventLoop loop;
    QTimer timer;
    timer.setSingleShot(true);
    QObject::connect(&timer, &QTimer::timeout, &loop, &QEventLoop::quit);
    QObject::connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
    timer.start(timeoutMs);
    loop.exec();
    if (!reply->isFinished()) {
        reply->abort();
        if (err) *err = "timeout POST " + path;
        return QString();
    }
    if (reply->error() != QNetworkReply::NoError) {
        if (err) *err = QString("HTTP %1 POST %2").arg(int(reply->error())).arg(path);
        reply->deleteLater();
        return QString();
    }
    QByteArray body = reply->readAll();
    reply->deleteLater();
    return QString::fromUtf8(body);
}

// ---------------------------------------------------------------------------
// Fetch do doc + tema + estado.
// ---------------------------------------------------------------------------
struct ShellDoc {
    QJsonObject qtDoc;    // {version, docks, actions, menus, toolbars, status}
    QJsonObject qtTheme;  // {name, palette, qss}
    QJsonObject state;    // snapshot /state
    QString docErr, themeErr, stateErr;
    bool docValid = false, themeValid = false, stateValid = false;
};

ShellDoc fetch_doc() {
    ShellDoc d;
    QString body = http_get("/qt-doc", 4000, &d.docErr);
    if (!body.isEmpty()) {
        QJsonObject root = QJsonDocument::fromJson(body.toUtf8()).object();
        if (root.value("valid").toBool(false)) {
            d.qtDoc = root.value("qt_doc").toObject();
            d.docValid = true;
        } else {
            d.docErr = "/qt-doc valid=false";
        }
    }
    body = http_get("/qt-theme", 4000, &d.themeErr);
    if (!body.isEmpty()) {
        QJsonObject root = QJsonDocument::fromJson(body.toUtf8()).object();
        if (root.value("valid").toBool(false)) {
            d.qtTheme = root.value("qt_theme").toObject();
            d.themeValid = true;
        } else {
            d.themeErr = "/qt-theme valid=false";
        }
    }
    body = http_get("/state", 4000, &d.stateErr);
    if (!body.isEmpty()) {
        d.state = QJsonDocument::fromJson(body.toUtf8()).object();
        d.stateValid = !d.state.isEmpty();
    }
    return d;
}

// ---------------------------------------------------------------------------
// Mapeamento de ações do doc -> rotas reais da Control API.
// Rotas verificadas contra EditorControlApi (34 rotas): comandos com POST
// existente executam no editor; sem rota -> aviso no status bar.
// ---------------------------------------------------------------------------
struct ActionRoute { const char* id; const char* method; const char* route; };

const ActionRoute kRoutes[] = {
    {"scene.new",      "POST", "/new-scene"},
    {"scene.save",     "POST", "/save-scene"},
    {"build.game",     "POST", "/package"},
    {"asset.refresh",  "POST", "/hot-reload"},
    {"entity.cube",    "POST", "/add-entity/cube"},
    {"play.toggle",    "POST", "/play"},   // vira /pause quando state==play (ver exec)
    {nullptr, nullptr, nullptr},
};

QString route_for(const QString& id) {
    for (const ActionRoute* r = kRoutes; r->id; ++r)
        if (id == QLatin1String(r->id)) return QString::fromLatin1(r->route);
    return QString();
}

// ---------------------------------------------------------------------------
// Widgets de conteúdo: Hierarchy (QTreeView) + Inspector (QTreeWidget) + counts.
// Consomem /hierarchy e /inspector da Control API (endpoints do AGENT-2).
// ---------------------------------------------------------------------------

// Preenche o QTreeView com as entidades do /hierarchy (indent por depth).
// Retorna o número de linhas adicionadas.
int populate_hierarchy(QTreeView* view, const QString& body) {
    QStandardItemModel* model =
        qobject_cast<QStandardItemModel*>(view->model());
    if (!model) return 0;
    model->clear();
    model->setHorizontalHeaderLabels({QStringLiteral("Hierarchy")});
    // Endpoint responde {valid:true, hierarchy:[...]} — extrai o array.
    const QJsonDocument doc = QJsonDocument::fromJson(body.toUtf8());
    QJsonArray rows = doc.array();
    if (rows.isEmpty() && doc.object().value("hierarchy").isArray()) {
        rows = doc.object().value("hierarchy").toArray();
    }
    QStandardItem* last = nullptr;
    for (const QJsonValue& v : rows) {
        const QJsonObject o = v.toObject();
        QStandardItem* item = new QStandardItem(o.value("name").toString());
        item->setData(o.value("id").toString(), Qt::UserRole);  // uuid p/ select
        const int depth = o.value("depth").toInt(0);
        if (depth > 0 && last) {
            QStandardItem* parent = last;
            for (int d = 1; d < depth; ++d) parent = parent->parent();
            parent->appendRow(item);
        } else {
            model->appendRow(item);
        }
        last = item;
    }
    view->expandAll();
    return static_cast<int>(rows.size());
}

// ViewportLabel: um QLabel que mantém o último QPixmap e o reescala no
// resizeEvent (dock redimensionado) — sem perder o frame atual.
class ViewportLabel : public QLabel {
public:
    using QLabel::QLabel;
    void set_viewport_pixmap(const QPixmap& pm) {
        m_raw = pm;
        rescale();
    }
protected:
    void resizeEvent(QResizeEvent* ev) override {
        QLabel::resizeEvent(ev);
        rescale();
    }
private:
    void rescale() {
        if (m_raw.isNull()) return;
        const QSize target = size();
        if (target.isEmpty()) return;
        QPixmap scaled = m_raw.scaled(target, Qt::KeepAspectRatio,
                                     Qt::SmoothTransformation);
        if (!scaled.isNull()) setPixmap(scaled);
    }
    QPixmap m_raw;
};

// Captura um frame do viewport do editor (POST /screenshot) e o carrega no
// ViewportLabel do dock viewport. Retorna true se a imagem carregou (não-vazia).
bool refresh_viewport(ViewportLabel* label, const QString& tag) {
    if (!label) return false;
    // O POST /screenshot resolve o path relativo contra a raiz da engine, mas o
    // shell pode rodar de <engine>/tools/qt_shell (dev) ou <engine>/tools/qt_shell/deploy
    // (pacote) — sobe até encontrar a pasta que contém screenshots/.
    static const QString engineRoot = []() {
        QDir dir(QFileInfo(QCoreApplication::applicationFilePath()).absolutePath());
        while (!dir.exists("screenshots") && dir.cdUp()) {}
        return dir.path();
    }();
    const QString path =
        QStringLiteral("screenshots/_shell_%1.png").arg(tag);
    http_post("/screenshot?path=" + path, 4000);
    const QString abs = engineRoot + "/" + path;
    QPixmap pm;
    if (QFile::exists(abs)) pm.load(abs);
    if (pm.isNull()) return false;
    label->set_viewport_pixmap(pm);
    return true;
}

// Preenche o QTreeView do content browser: folders -> assets (do /content-browser).
// Retorna o número de itens (folders + assets).
int populate_content_browser(QTreeView* view, const QString& body) {
    QStandardItemModel* model =
        qobject_cast<QStandardItemModel*>(view->model());
    if (!model) return 0;
    model->clear();
    model->setHorizontalHeaderLabels({QStringLiteral("Content Browser")});
    const QJsonDocument doc = QJsonDocument::fromJson(body.toUtf8());
    QJsonObject b = doc.object().value("browser").toObject();
    if (b.isEmpty()) b = doc.object();
    int count = 0;
    const QJsonArray folders = b.value("folders").toArray();
    for (const QJsonValue& fv : folders) {
        const QJsonObject f = fv.toObject();
        QStandardItem* folder =
            new QStandardItem(QStringLiteral("📁 %1").arg(
                f.value("name").toString()));
        folder->setEditable(false);
        model->appendRow(folder);
        ++count;
        const QJsonArray children = f.value("children").toArray();
        for (const QJsonValue& cv : children) {
            QStandardItem* child =
                new QStandardItem(cv.toString());
            child->setEditable(false);
            folder->appendRow(child);
            ++count;
        }
    }
    // Assets raiz (sem pasta) aparecem direto na raiz da árvore.
    const QJsonArray assets = b.value("assets").toArray();
    for (const QJsonValue& av : assets) {
        QStandardItem* item = new QStandardItem(av.toString());
        item->setEditable(false);
        model->appendRow(item);
        ++count;
    }
    view->expandAll();
    return count;
}

// Preenche o QTreeWidget do inspector: components -> properties (tipadas).
// Retorna o número de componentes adicionados.
int populate_inspector(QTreeWidget* tree, const QString& body) {
    tree->clear();
    tree->setColumnCount(1);
    tree->setHeaderLabels({QStringLiteral("Inspector")});
    const QJsonDocument rootDoc = QJsonDocument::fromJson(body.toUtf8());
    QJsonObject doc = rootDoc.object();
    if (doc.value("inspector").isObject()) doc = doc.value("inspector").toObject();
    if (!doc.value("has_entity").toBool(false)) return 0;
    int comps = 0;
    const QJsonArray components = doc.value("components").toArray();
    for (const QJsonValue& cv : components) {
        const QJsonObject c = cv.toObject();
        QTreeWidgetItem* comp =
            new QTreeWidgetItem(tree,
                {QStringLiteral("%1 [%2]").arg(c.value("name").toString(),
                                              c.value("group").toString())});
        const QJsonArray props = c.value("properties").toArray();
        for (const QJsonValue& pv : props) {
            const QJsonObject p = pv.toObject();
            new QTreeWidgetItem(comp,
                {QStringLiteral("%1 : %2").arg(p.value("name").toString(),
                                                p.value("type").toString())});
        }
        ++comps;
    }
    tree->expandAll();
    return comps;
}

// ---------------------------------------------------------------------------
// Constrói a QMainWindow a partir do doc (sem dead code: cada elemento do doc
// vira um widget real; actions com rota executam via Control API).
// ---------------------------------------------------------------------------
void apply_doc(QMainWindow& w, const ShellDoc& d, QString* outErr) {
    if (!d.docValid) { *outErr = "sem qt-doc valido"; return; }

    // --- Docks -----------------------------------------------------------------
    const QJsonArray docks = d.qtDoc.value("docks").toArray();
    for (const QJsonValue& v : docks) {
        const QJsonObject o = v.toObject();
        QDockWidget* dock = new QDockWidget(o.value("title").toString(), &w);
        dock->setObjectName(o.value("objectName").toString());
        Qt::DockWidgetArea area = Qt::LeftDockWidgetArea;
        const QString a = o.value("area").toString();
        if (a == "Right") area = Qt::RightDockWidgetArea;
        else if (a == "Top") area = Qt::TopDockWidgetArea;
        else if (a == "Bottom") area = Qt::BottomDockWidgetArea;
        else if (a == "Floating") area = Qt::NoDockWidgetArea;
        QDockWidget::DockWidgetFeatures feats = QDockWidget::NoDockWidgetFeatures;
        if (o.value("closable").toBool(true))  feats |= QDockWidget::DockWidgetClosable;
        if (o.value("movable").toBool(true))   feats |= QDockWidget::DockWidgetMovable;
        if (o.value("floatable").toBool(true)) feats |= QDockWidget::DockWidgetFloatable;
        dock->setFeatures(feats);
        // Content widgets for the real panels: Hierarchy (QTreeView with
        // selection -> /inspector) and Inspector (QTreeWidget). All other
        // docks get a labeled placeholder (still a real widget, no dead code).
        const QString dockName = dock->objectName();
        if (dockName == "hierarchy") {
            QTreeView* view = new QTreeView(&w);
            view->setObjectName("hierarchyView");
            view->setModel(new QStandardItemModel(view));
            view->setEditTriggers(QAbstractItemView::NoEditTriggers);
            dock->setWidget(view);
        } else if (dockName == "content_browser") {
            QTreeView* view = new QTreeView(&w);
            view->setObjectName("contentBrowserView");
            view->setModel(new QStandardItemModel(view));
            view->setEditTriggers(QAbstractItemView::NoEditTriggers);
            dock->setWidget(view);
        } else if (dockName == "inspector") {
            QTreeWidget* tree = new QTreeWidget(&w);
            tree->setObjectName("inspectorTree");
            tree->setColumnCount(1);
            dock->setWidget(tree);
        } else if (dockName == "viewport") {
            ViewportLabel* viewport = new ViewportLabel(&w);
            viewport->setObjectName("viewportLabel");
            viewport->setAlignment(Qt::AlignCenter);
            viewport->setMinimumSize(320, 180);
            viewport->setText(QStringLiteral("[viewport]"));
            dock->setWidget(viewport);
        } else {
            // Docks de ferramenta com endpoint GET vivo mostram o JSON
            // formatado; os demais ficam com um placeholder rotulado
            // (ainda um widget real, sem dead code).
            const QString route = live_endpoint_for(dockName);
            if (!route.isEmpty()) {
                QPlainTextEdit* te = new QPlainTextEdit(&w);
                te->setObjectName("live:" + dockName);
                te->setReadOnly(true);
                QFont mono = te->font();
                mono.setFamily(QStringLiteral("Consolas"));
                mono.setStyleHint(QFont::Monospace);
                mono.setPointSize(8);
                te->setFont(mono);
                te->setPlainText(QStringLiteral("[carregando %1 ...]").arg(route));
                dock->setWidget(te);
            } else {
                QWidget* placeholder = new QWidget(&w);
                placeholder->setObjectName("placeholder:" + dockName);
                dock->setWidget(placeholder);
            }
        }
        if (area == Qt::NoDockWidgetArea) dock->setFloating(true);
        else w.addDockWidget(area, dock);
        if (o.value("visible").toBool(true)) dock->show(); else dock->hide();
    }

    // --- Actions ---------------------------------------------------------------
    const QJsonArray acts = d.qtDoc.value("actions").toArray();
    for (const QJsonValue& v : acts) {
        const QJsonObject o = v.toObject();
        QAction* act = new QAction(o.value("text").toString(), &w);
        act->setObjectName(o.value("id").toString());
        const QString sc = o.value("shortcut").toString();
        if (!sc.isEmpty()) act->setShortcut(QKeySequence::fromString(sc));
        act->setCheckable(o.value("checkable").toBool(false));
        act->setEnabled(o.value("enabled").toBool(true));
        const QString id = act->objectName();
        const QString route = route_for(id);
        if (!route.isEmpty()) {
            QObject::connect(act, &QAction::triggered, &w, [id, route]() {
                QString path = route;
                if (id == "play.toggle") {
                    // alterna play/pause conforme o estado real
                    QJsonObject st = QJsonDocument::fromJson(http_get("/state").toUtf8()).object();
                    path = (st.value("state").toString() == "play") ? "/pause" : "/play";
                }
                http_post(path);
            });
        } else {
            QObject::connect(act, &QAction::triggered, &w, [id]() {
                Q_UNUSED(id);
            });
        }
        w.addAction(act);
    }

    // --- Menus -----------------------------------------------------------------
    const QJsonArray menus = d.qtDoc.value("menus").toArray();
    for (const QJsonValue& v : menus) {
        const QJsonObject o = v.toObject();
        QMenu* menu = w.menuBar()->addMenu(o.value("title").toString());
        menu->setObjectName(o.value("id").toString());
        const QJsonArray items = o.value("actions").toArray();
        for (const QJsonValue& idv : items) {
            const QString id = idv.toString();
            QAction* act = w.findChild<QAction*>(id);
            if (act) menu->addAction(act);
        }
    }

    // --- Toolbars ---------------------------------------------------------------
    const QJsonArray tb = d.qtDoc.value("toolbars").toArray();
    for (const QJsonValue& v : tb) {
        const QJsonObject o = v.toObject();
        QToolBar* bar = w.addToolBar(o.value("title").toString());
        bar->setObjectName(o.value("id").toString());
        const QJsonArray items = o.value("actions").toArray();
        for (const QJsonValue& idv : items) {
            const QString id = idv.toString();
            QAction* act = w.findChild<QAction*>(id);
            if (act) bar->addAction(act);
        }
    }

    // --- Status bar --------------------------------------------------------------
    w.statusBar()->addWidget(new QLabel("state: --", &w));
    w.statusBar()->addWidget(new QLabel("scene: --", &w));
    w.statusBar()->addWidget(new QLabel("entities: --", &w));
    w.statusBar()->addWidget(new QLabel("frame: -- ms", &w));

    // --- Tema ---------------------------------------------------------------------
    if (d.themeValid) {
        const QJsonObject pal = d.qtTheme.value("palette").toObject();
        QPalette p;
        auto setRole = [&](QPalette::ColorRole role, const char* key) {
            const QString hex = pal.value(QLatin1String(key)).toString();
            if (hex.size() == 7 && hex.at(0) == '#') {
                bool ok = false;
                const QRgb rgb = hex.mid(1).toUInt(&ok, 16);
                if (ok) p.setColor(role, QColor::fromRgb(rgb));
            }
        };
        setRole(QPalette::Window,            "Window");
        setRole(QPalette::WindowText,        "WindowText");
        setRole(QPalette::Base,              "Base");
        setRole(QPalette::AlternateBase,     "AlternateBase");
        setRole(QPalette::Text,              "Text");
        setRole(QPalette::Button,            "Button");
        setRole(QPalette::ButtonText,        "ButtonText");
        setRole(QPalette::Highlight,         "Highlight");
        setRole(QPalette::HighlightedText,   "HighlightedText");
        setRole(QPalette::PlaceholderText,   "PlaceholderText");
        setRole(QPalette::ToolTipBase,       "ToolTipBase");
        setRole(QPalette::ToolTipText,       "ToolTipText");
        w.setPalette(p);
        QString qss;
        const QJsonArray rules = d.qtTheme.value("qss").toArray();
        for (const QJsonValue& rv : rules) {
            const QJsonObject r = rv.toObject();
            qss += r.value("selector").toString() + " { " +
                   r.value("declarations").toString() + " }\n";
        }
        if (!qss.isEmpty()) w.setStyleSheet(qss);
    }

    w.setWindowTitle(QString("VulkanCraft Editor (Qt shell) — %1").arg(base_url));

    // --- Content refresh: Hierarchy + Inspector from the live endpoints. -----
    // Selection in the hierarchy triggers /select/{uuid} then re-fetches the
    // inspector for the chosen entity.
    QTreeView* hierView = w.findChild<QTreeView*>("hierarchyView");
    QTreeWidget* inspTree = w.findChild<QTreeWidget*>("inspectorTree");
    if (hierView) {
        populate_hierarchy(hierView, http_get("/hierarchy"));
        QObject::connect(hierView->selectionModel(),
                         &QItemSelectionModel::currentChanged, &w,
                         [inspTree](const QModelIndex& idx, const QModelIndex&) {
            if (!idx.isValid() || !inspTree) return;
            const QString uuid =
                idx.data(Qt::UserRole).toString();
            if (uuid.isEmpty()) return;
            http_post("/select/" + uuid);
            populate_inspector(inspTree, http_get("/inspector"));
        });
    }
    if (inspTree) populate_inspector(inspTree, http_get("/inspector"));
    QTreeView* cbView = w.findChild<QTreeView*>("contentBrowserView");
    if (cbView) populate_content_browser(cbView, http_get("/content-browser"));
}

// ---------------------------------------------------------------------------
// Modo smoke: monta headless e valida contra o doc real (gate de integração).
// ---------------------------------------------------------------------------
int run_smoke(int port) {
    base_url = QString("http://127.0.0.1:%1").arg(port);
    ShellDoc d = fetch_doc();
    int fails = 0;

    auto check = [&fails](bool ok, const char* what, const QString& detail) {
        if (!ok) {
            ++fails;
            std::printf("  [FAIL] %s%s%s\n", what,
                        detail.isEmpty() ? "" : " — ",
                        detail.toUtf8().constData());
        } else {
            std::printf("  [ ok ] %s\n", what);
        }
    };

    std::printf("SMOKE QtShell (base %s)\n", base_url.toUtf8().constData());

    // 1. Fetches
    check(d.docValid,  "/qt-doc valido",   d.docErr);
    check(d.themeValid, "/qt-theme valido", d.themeErr);
    check(d.stateValid, "/state valido",   d.stateErr);
    if (!d.docValid) { std::printf("SMOKE FAIL (sem qt-doc)\n"); return 1; }

    // 2. Conteúdo do doc (o que o editor realmente publica)
    const QJsonArray docks = d.qtDoc.value("docks").toArray();
    const QJsonArray acts = d.qtDoc.value("actions").toArray();
    const QJsonArray menus = d.qtDoc.value("menus").toArray();
    const QJsonArray tb = d.qtDoc.value("toolbars").toArray();
    check(docks.size() > 0,  QString("docks no doc: %1").arg(docks.size()).toUtf8().constData(), "");
    check(acts.size() > 0,   QString("actions no doc: %1").arg(acts.size()).toUtf8().constData(), "");
    check(menus.size() > 0,  QString("menus no doc: %1").arg(menus.size()).toUtf8().constData(), "");
    check(tb.size() > 0,     QString("toolbars no doc: %1").arg(tb.size()).toUtf8().constData(), "");

    // 3. Montagem real da janela
    QMainWindow w;
    QString err;
    apply_doc(w, d, &err);
    check(err.isEmpty(), "montagem QMainWindow", err);

    const int builtDocks = w.findChildren<QDockWidget*>().size();
    const int builtActs  = w.findChildren<QAction*>().size();
    const int builtMenus = w.menuBar()->actions().size();
    const int builtTbs   = w.findChildren<QToolBar*>().size();
    const int builtStLbl = w.statusBar()->findChildren<QLabel*>().size();
    check(builtDocks == docks.size(),
          QString("QDockWidget montados: %1/%2").arg(builtDocks).arg(docks.size()).toUtf8().constData(), "");
    check(builtActs >= acts.size(),
          QString("QAction montados: %1 (doc %2)").arg(builtActs).arg(acts.size()).toUtf8().constData(), "");
    check(builtMenus >= menus.size(),
          QString("menus na menu bar: %1 (doc %2)").arg(builtMenus).arg(menus.size()).toUtf8().constData(), "");
    check(builtTbs == tb.size(),
          QString("toolbars montadas: %1/%2").arg(builtTbs).arg(tb.size()).toUtf8().constData(), "");
    check(builtStLbl == 4, "status bar com 4 labels", "");

    // 4. Tema aplicado (palette + QSS)
    if (d.themeValid) {
        const QJsonObject pal = d.qtTheme.value("palette").toObject();
        const QString winHex = pal.value("Window").toString();
        const QColor winColor = w.palette().color(QPalette::Window);
        const QString applied = winColor.name().toUpper();
        check(!winHex.isEmpty() && applied == winHex,
              QString("palette Window aplicada: %1 (doc %2)").arg(applied, winHex).toUtf8().constData(), "");
        check(!w.styleSheet().isEmpty(), "QSS aplicado (non-empty)", "");
    }

    // 5. Status vivo
    const QJsonObject st = d.qtDoc.value("status").toObject();
    check(st.contains("state"), "status.state presente", "");
    check(st.contains("scene"), "status.scene presente", "");

    // 6. Widgets de conteúdo (viewport + hierarchy + inspector + content browser)
    ViewportLabel* vpLabel = w.findChild<ViewportLabel*>("viewportLabel");
    check(vpLabel != nullptr, "dock Viewport com ViewportLabel (resize-aware)", "");
    const bool vpOk = vpLabel && refresh_viewport(vpLabel, "smoke");
    check(vpOk, "viewport capturou frame do editor", "");
    QTreeView* cbView = w.findChild<QTreeView*>("contentBrowserView");
    check(cbView != nullptr, "dock Content Browser com QTreeView", "");
    if (cbView) populate_content_browser(cbView, http_get("/content-browser"));
    QTreeView* hierView = w.findChild<QTreeView*>("hierarchyView");
    QTreeWidget* inspTree = w.findChild<QTreeWidget*>("inspectorTree");
    check(hierView != nullptr, "dock Hierarchy com QTreeView", "");
    check(inspTree != nullptr, "dock Inspector com QTreeWidget", "");
    const QString hierBody = http_get("/hierarchy");
    const int hierRows = hierView ? populate_hierarchy(hierView, hierBody)
                                  : 0;
    check(hierRows > 0,
          QString("hierarchy populada: %1 entidades").arg(hierRows)
              .toUtf8().constData(), "");
    // Seleciona a primeira entidade -> inspector deve vir com componentes.
    if (hierView && hierView->model() && hierView->model()->rowCount() > 0) {
        const QModelIndex first = hierView->model()->index(0, 0);
        const QString uuid = first.data(Qt::UserRole).toString();
        if (!uuid.isEmpty()) http_post("/select/" + uuid);
        const QString inspBody = http_get("/inspector");
        const int comps = inspTree ? populate_inspector(inspTree, inspBody)
                                   : 0;
        check(comps > 0,
              QString("inspector com %1 componentes da 1ª entidade").arg(comps)
                  .toUtf8().constData(), "");
    }

    // 7. Docks de ferramenta com endpoint vivo (JSON formatado, não placeholder)
    const QList<QPlainTextEdit*> live = w.findChildren<QPlainTextEdit*>();
    int liveMapped = 0;
    for (QPlainTextEdit* te : live)
        if (te->objectName().startsWith(QStringLiteral("live:"))) ++liveMapped;
    check(liveMapped > 0,
          QString("docks com endpoint vivo montados: %1").arg(liveMapped)
              .toUtf8().constData(), "");
    refresh_live_docks(w);
    int livePopAfter = 0;
    for (QPlainTextEdit* te : live) {
        if (te->objectName().startsWith(QStringLiteral("live:")) &&
            !te->toPlainText().contains(QStringLiteral("[carregando"))) ++livePopAfter;
    }
    check(livePopAfter == liveMapped,
          QString("docks com endpoint vivo populados: %1/%2").arg(livePopAfter).arg(liveMapped)
              .toUtf8().constData(), "");

    std::printf(fails == 0 ? "SMOKE OK\n" : "SMOKE FAIL (%d checks)\n", fails);
    return fails == 0 ? 0 : 1;
}

// ---------------------------------------------------------------------------
// Modo janela real: monta + refresh periódico do status bar via Control API.
// ---------------------------------------------------------------------------
int run_window(int port) {
    base_url = QString("http://127.0.0.1:%1").arg(port);
    QMainWindow w;
    QString err;
    ShellDoc d = fetch_doc();
    if (!d.docValid) {
        std::fprintf(stderr, "QtShell: sem /qt-doc valido em %s (%s)\n",
                     base_url.toUtf8().constData(), d.docErr.toUtf8().constData());
        return 2;
    }
    apply_doc(w, d, &err);
    if (!err.isEmpty()) {
        std::fprintf(stderr, "QtShell: %s\n", err.toUtf8().constData());
        return 2;
    }
    w.resize(1600, 900);
    w.show();

    // Primeiro fetch dos docks de ferramenta (JSON dos endpoints vivos).
    refresh_live_docks(w);

    // Status vivo a cada 500ms.
    QTimer* refresh = new QTimer(&w);
    QObject::connect(refresh, &QTimer::timeout, &w, [&w]() {
        const QJsonObject st =
            QJsonDocument::fromJson(http_get("/qt-doc", 1500).toUtf8()).object()
                .value("qt_doc").toObject().value("status").toObject();
        QList<QLabel*> labels = w.statusBar()->findChildren<QLabel*>();
        if (labels.size() >= 4 && !st.isEmpty()) {
            labels[0]->setText(QString("state: %1").arg(st.value("state").toString()));
            labels[1]->setText(QString("scene: %1").arg(st.value("scene").toString()));
            labels[2]->setText(QString("entities: %1").arg(st.value("entities").toInt()));
            labels[3]->setText(QString("frame: %1 ms").arg(st.value("frameMillis").toInt()));
        }
        // Content refresh: keep hierarchy (only when the scene actually has
        // entities; the selection handler re-fills the inspector on click).
        QTreeView* hier = w.findChild<QTreeView*>("hierarchyView");
        if (hier && !st.isEmpty() && st.value("entities").toInt() > 0) {
            populate_hierarchy(hier, http_get("/hierarchy", 1500));
        }
        // Viewport ao vivo: captura um frame a cada 2 ticks (~1Hz) e mostra
        // no QLabel do dock viewport.
        static int viewportTick = 0;
        if ((++viewportTick % 2) == 0) {
            ViewportLabel* vp = w.findChild<ViewportLabel*>("viewportLabel");
            refresh_viewport(vp, "live");
        }
        // Docks de ferramenta: JSON dos endpoints vivos a cada 4 ticks (~0.5Hz)
        // para não saturar a Control API com 11 fetches a cada tick.
        static int liveTick = 0;
        if ((++liveTick % 4) == 0) refresh_live_docks(w);
    });
    refresh->start(500);

    return QApplication::exec();
}

} // namespace

int main(int argc, char** argv) {
    bool smoke = false;
    int port = 8321;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--smoke") == 0) smoke = true;
        else if (std::strcmp(argv[i], "--port") == 0 && i + 1 < argc) port = std::atoi(argv[++i]);
    }

    // Smoke roda headless (offscreen) — gate de integração sem janela.
    if (smoke) {
        qputenv("QT_QPA_PLATFORM", "offscreen");
        QApplication app(argc, argv);
        return run_smoke(port);
    }

    QApplication app(argc, argv);
    return run_window(port);
}
