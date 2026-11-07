#include <QtCore>
#include <QtGui>
#include <qpa/qplatformintegrationplugin.h>
#include <qpa/qplatformintegration.h>
#include <qpa/qplatformwindow.h>
#include <qpa/qplatformscreen.h>
#include <qpa/qplatformbackingstore.h>
#include <qpa/qplatformcursor.h>
#include <qpa/qwindowsysteminterface.h>
#include <QtGui/private/qfreetypefontdatabase_p.h>
#include <QtGui/private/qpixmap_raster_p.h>
#include <windows.h>
#include <atomic>
#include <cstring>
#include <qt_raster_protocol.hpp>
#include <limits>
#include <mutex>

namespace qb = sogen::qt_raster;

namespace
{
    HANDLE device = INVALID_HANDLE_VALUE;
    uint32_t sequence = 0;
    uint64_t session = 0;
    std::recursive_mutex exchange_mutex;
    QHash<uint32_t, QWindow*> windows;
    qreal scale = 1;
    QRect screenGeometry(0, 0, 1280, 900);

    qb::Packet exchange(qb::Packet p, QByteArrayView data = {})
    {
        const std::lock_guard lock(exchange_mutex);
        if (sequence == UINT32_MAX || data.size() > qb::max_payload)
        {
            qFatal("BRIDGE_REQUEST_LIMIT");
        }
        p.seq = ++sequence;
        p.session = session;
        p.size = uint32_t(data.size());
        QByteArray request(qsizetype(sizeof(p)) + data.size(), Qt::Uninitialized);
        std::memcpy(request.data(), &p, sizeof(p));
        if (!data.empty())
        {
            std::memcpy(request.data() + sizeof(p), data.data(), size_t(data.size()));
        }
        char reply[4096]{};
        DWORD received = 0;
        if (!DeviceIoControl(device, qb::ioctl, request.data(), DWORD(request.size()), reply, sizeof(reply), &received, nullptr))
        {
            qFatal("BRIDGE_IO_FAILED %lu", GetLastError());
        }
        if (received < sizeof(qb::Packet))
        {
            qFatal("BRIDGE_SHORT_REPLY");
        }
        qb::Packet result;
        std::memcpy(&result, reply, sizeof(result));
        if (!qb::valid(result, received) || result.seq != p.seq)
        {
            qFatal("BRIDGE_INVALID_REPLY");
        }
        if (p.op == qb::Hello)
        {
            if (result.op != qb::Hello || result.session == 0)
            {
                qFatal("BRIDGE_INVALID_SESSION");
            }
            session = result.session;
        }
        else if (result.session != session)
        {
            qFatal("BRIDGE_STALE_SESSION");
        }
        if (result.op == qb::Key && windows.contains(result.id))
        {
            QWindowSystemInterface::handleKeyEvent(windows.value(result.id), QEvent::Type(result.a[0]), result.a[1],
                                                   Qt::KeyboardModifiers(result.a[2]),
                                                   QString::fromUtf8(reply + sizeof(result), int(result.size)), result.a[3]);
            result.op = qb::Ack;
        }
        return result;
    }

    void send(qb::Op op, uint32_t id, std::initializer_list<int32_t> args = {}, QByteArrayView data = {})
    {
        if (args.size() > qb::Packet{}.a.size())
        {
            qFatal("BRIDGE_ARGUMENT_LIMIT");
        }
        qb::Packet p;
        p.op = op;
        p.id = id;
        std::copy(args.begin(), args.end(), p.a.begin());
        exchange(p, data);
    }

    class Cursor : public QPlatformCursor
    {
        void changeCursor(QCursor* cursor, QWindow* w) override
        {
            if (w && cursor)
            {
                send(qb::Cursor, uint32_t(w->winId()), {int(cursor->shape())});
            }
        }
    };

    class Screen : public QPlatformScreen
    {
        Cursor cursor_;

      public:
        QRect geometry() const override
        {
            return screenGeometry;
        }

        int depth() const override
        {
            return 32;
        }

        QImage::Format format() const override
        {
            return QImage::Format_ARGB32_Premultiplied;
        }

        qreal devicePixelRatio() const override
        {
            return 1;
        }

        QDpi logicalDpi() const override
        {
            return {96 * scale, 96 * scale};
        }

        QDpi logicalBaseDpi() const override
        {
            return {96, 96};
        }

        QPlatformCursor* cursor() const override
        {
            return const_cast<Cursor*>(&cursor_);
        }
    };

    Screen* screen = nullptr;

    class Window : public QPlatformWindow
    {
        uint32_t id_;
        bool visible_ = false;

      public:
        explicit Window(QWindow* w)
            : QPlatformWindow(w)
        {
            static uint32_t next = 1;
            if (!next)
            {
                qFatal("BRIDGE_WINDOW_ID_LIMIT");
            }
            id_ = next++;
            windows.insert(id_, w);
            const auto g = windowGeometry();
            const uint32_t parent = w->transientParent() ? uint32_t(w->transientParent()->winId()) : 0;
            send(qb::Create, id_, {g.x(), g.y(), g.width(), g.height(), int(w->type()), int(w->modality()), int(parent), int(scale * 1000)},
                 w->title().toUtf8());
        }

        ~Window() override
        {
            send(qb::Destroy, id_);
            windows.remove(id_);
        }

        WId winId() const override
        {
            return WId(id_);
        }

        void setGeometry(const QRect& r) override
        {
            QPlatformWindow::setGeometry(r);
            send(qb::Geometry, id_, {r.x(), r.y(), r.width(), r.height()});
            QWindowSystemInterface::handleGeometryChange(window(), r);
        }

        void setVisible(bool v) override
        {
            visible_ = v;
            send(qb::Visible, id_, {v, int(window()->modality())});
            QWindowSystemInterface::handleExposeEvent(window(), v ? QRegion(QRect(QPoint(), geometry().size())) : QRegion());
            if (v)
            {
                requestActivateWindow();
            }
        }

        bool isExposed() const override
        {
            return visible_;
        }

        void setWindowTitle(const QString& s) override
        {
            send(qb::Title, id_, {}, s.toUtf8());
        }

        void raise() override
        {
            send(qb::Raise, id_, {1});
        }

        void lower() override
        {
            send(qb::Raise, id_, {0});
        }

        void requestActivateWindow() override
        {
            send(qb::Focus, id_, {1});
            QWindowSystemInterface::handleFocusWindowChanged(window());
        }

        bool setKeyboardGrabEnabled(bool) override
        {
            return true;
        }

        bool setMouseGrabEnabled(bool) override
        {
            return true;
        }

        qreal devicePixelRatio() const override
        {
            return 1;
        }
    };

    class BackingStore : public QPlatformBackingStore
    {
        QImage image_;

      public:
        explicit BackingStore(QWindow* w)
            : QPlatformBackingStore(w)
        {
        }

        QPaintDevice* paintDevice() override
        {
            return &image_;
        }

        void resize(const QSize& s, const QRegion&) override
        {
            image_ = QImage(s, QImage::Format_ARGB32_Premultiplied);
            image_.fill(Qt::transparent);
        }

        void flush(QWindow* w, const QRegion& region, const QPoint&) override
        {
            if (image_.isNull())
            {
                return;
            }
            auto r = region.boundingRect();
            send(qb::Frame, uint32_t(w->winId()),
                 {image_.width(), image_.height(), int(image_.bytesPerLine()), int(scale * 1000), r.x(), r.y(), r.width(), r.height()},
                 QByteArrayView(reinterpret_cast<const char*>(image_.constBits()), image_.sizeInBytes()));
        }
    };

    class Dispatcher : public QAbstractEventDispatcher
    {
        struct Timer
        {
            qint64 interval, due;
            Qt::TimerType type;
            QPointer<QObject> object;
            bool active = false;
        };

        QHash<int, Timer> timers;
        QElapsedTimer clock;
        std::atomic<bool> interrupted{false};
        std::atomic<bool> woken{false};

      public:
        Dispatcher()
        {
            clock.start();
            qInfo("BRIDGE_DISPATCHER_READY");
        }

        bool processEvents(QEventLoop::ProcessEventsFlags flags) override
        {
            interrupted = false;
            woken = false;
            emit awake();
            QCoreApplication::sendPostedEvents();
            bool worked = QWindowSystemInterface::sendWindowSystemEvents(flags);
            if (!(flags & QEventLoop::ExcludeUserInputEvents))
            {
                for (int i = 0; i < 64 && !interrupted; ++i)
                {
                    qb::Packet request;
                    request.op = qb::Poll;
                    const auto p = exchange(request);
                    if (p.op == qb::Op{})
                    {
                        break;
                    }
                    auto* w = windows.value(p.id, nullptr);
                    if (p.op == qb::Screen)
                    {
                        scale = qBound(1., p.a[2] / 1000., 3.);
                        screenGeometry = QRect(0, 0, p.a[0], p.a[1]);
                        QWindowSystemInterface::handleScreenGeometryChange(screen->screen(), screenGeometry, screenGeometry);
                        QWindowSystemInterface::handleScreenLogicalDotsPerInchChange(screen->screen(), 96 * scale, 96 * scale);
                        for (auto* window : windows)
                        {
                            QWindowSystemInterface::handleWindowDevicePixelRatioChanged(window);
                        }
                    }
                    else if (w)
                    {
                        const QPointF local(p.a[0], p.a[1]);
                        const QPointF global = local + w->handle()->geometry().topLeft();
                        switch (p.op)
                        {
                        case qb::Mouse:
                            QWindowSystemInterface::handleMouseEvent(w, local, global, Qt::MouseButtons(p.a[2]), Qt::MouseButton(p.a[3]),
                                                                     QEvent::Type(p.a[4]), Qt::KeyboardModifiers(p.a[5]));
                            break;
                        case qb::Wheel:
                            QWindowSystemInterface::handleWheelEvent(w, local, global, QPoint(), QPoint(p.a[2], p.a[3]),
                                                                     Qt::KeyboardModifiers(p.a[4]));
                            break;
                        case qb::Focus:
                            QWindowSystemInterface::handleFocusWindowChanged(p.a[0] ? w : nullptr);
                            break;
                        case qb::Close:
                            QWindowSystemInterface::handleCloseEvent(w);
                            break;
                        case qb::Resize:
                            w->handle()->setGeometry(QRect(p.a[0], p.a[1], p.a[2], p.a[3]));
                            QWindowSystemInterface::handleExposeEvent(w, QRegion(QRect(0, 0, p.a[2], p.a[3])));
                            break;
                        default:
                            break;
                        }
                    }
                    worked = true;
                }
            }
            const auto now = clock.elapsed();
            for (int id : timers.keys())
            {
                if (!timers.contains(id))
                {
                    continue;
                }
                auto& t = timers[id];
                if (!t.active && t.object && t.due <= now)
                {
                    QPointer<QObject> target = t.object;
                    t.due = now + qMax<qint64>(1, t.interval);
                    t.active = true;
                    QTimerEvent event(id);
                    QCoreApplication::sendEvent(target, &event);
                    if (timers.contains(id))
                    {
                        timers[id].active = false;
                    }
                    worked = true;
                }
            }
            worked = QWindowSystemInterface::sendWindowSystemEvents(flags) || worked;
            if (!worked && (flags & QEventLoop::WaitForMoreEvents) && !interrupted && !woken)
            {
                emit aboutToBlock();
                Sleep(1);
            }
            return worked;
        }

        void registerSocketNotifier(QSocketNotifier*) override
        {
            qWarning("BRIDGE_UNSUPPORTED socket notifier");
        }

        void unregisterSocketNotifier(QSocketNotifier*) override
        {
        }

        void registerTimer(int id, qint64 interval, Qt::TimerType type, QObject* object) override
        {
            timers.insert(id, {interval, clock.elapsed() + interval, type, object});
        }

        bool unregisterTimer(int id) override
        {
            return timers.remove(id) > 0;
        }

        bool unregisterTimers(QObject* o) override
        {
            bool found = false;
            for (int id : timers.keys())
            {
                if (timers[id].object == o)
                {
                    found = unregisterTimer(id) || found;
                }
            }
            return found;
        }

        QList<TimerInfo> registeredTimers(QObject* o) const override
        {
            QList<TimerInfo> result;
            for (auto i = timers.begin(); i != timers.end(); ++i)
            {
                if (i->object == o)
                {
                    result.append(TimerInfo(i.key(), int(i->interval), i->type));
                }
            }
            return result;
        }

        int remainingTime(int id) override
        {
            return timers.contains(id) ? int(qMax<qint64>(0, timers[id].due - clock.elapsed())) : -1;
        }

        void wakeUp() override
        {
            woken = true;
        }

        void interrupt() override
        {
            interrupted = true;
        }
    };

    class Integration : public QPlatformIntegration
    {
        QFreeTypeFontDatabase fonts;

      public:
        Integration()
        {
            device = CreateFileW(L"\\\\.\\SogenQtBridge", GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
            if (device == INVALID_HANDLE_VALUE)
            {
                qFatal("BRIDGE_OPEN_FAILED %lu", GetLastError());
            }
            qb::Packet p;
            p.op = qb::Hello;
            auto hello = exchange(p);
            scale = qBound(1., hello.a[2] / 1000., 3.);
            if (hello.a[0] > 0)
            {
                screenGeometry = QRect(0, 0, hello.a[0], hello.a[1]);
            }
            qputenv("QT_QPA_FONTDIR", (QCoreApplication::applicationDirPath() + "/fonts").toUtf8());
            screen = new Screen;
            QWindowSystemInterface::handleScreenAdded(screen, true);
            qInfo("BRIDGE_PLUGIN_READY qt=%s", qVersion());
        }

        ~Integration() override
        {
            QWindowSystemInterface::handleScreenRemoved(screen);
            screen = nullptr;
            if (windows.isEmpty())
            {
                qb::Packet end;
                end.op = qb::End;
                exchange(end);
            }
            CloseHandle(device);
            device = INVALID_HANDLE_VALUE;
        }

        bool hasCapability(Capability c) const override
        {
            if (c == MultipleWindows || c == ThreadedPixmaps)
            {
                return true;
            }
            if (c == OpenGL || c == RhiBasedRendering || c == ThreadedOpenGL)
            {
                return false;
            }
            return QPlatformIntegration::hasCapability(c);
        }

        QPlatformWindow* createPlatformWindow(QWindow* w) const override
        {
            return new Window(w);
        }

        QPlatformBackingStore* createPlatformBackingStore(QWindow* w) const override
        {
            return new BackingStore(w);
        }

        QPlatformPixmap* createPlatformPixmap(QPlatformPixmap::PixelType type) const override
        {
            return new QRasterPlatformPixmap(type);
        }

        QAbstractEventDispatcher* createEventDispatcher() const override
        {
            return new Dispatcher;
        }

        QPlatformFontDatabase* fontDatabase() const override
        {
            return const_cast<QFreeTypeFontDatabase*>(&fonts);
        }
    };
}

class BridgePlugin : public QPlatformIntegrationPlugin
{
    Q_OBJECT
    Q_PLUGIN_METADATA(IID QPlatformIntegrationFactoryInterface_iid FILE "bridge.json")
  public:
    QPlatformIntegration* create(const QString& key, const QStringList&) override
    {
        return key == "bridge" ? new Integration : nullptr;
    }
};

#include "plugin.moc"
