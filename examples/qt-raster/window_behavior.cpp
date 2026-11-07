#include <QApplication>
#include <QDialog>
#include <QHBoxLayout>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QPointer>
#include <QPushButton>
#include <QShortcut>
#include <QTimer>
#include <QVBoxLayout>
#include <QWindow>

#include <cstdio>

namespace
{
    void emit_json(const char* kind, const QJsonObject& object)
    {
        const auto text = QJsonDocument(object).toJson(QJsonDocument::Compact);
        std::printf("%s %s\n", kind, text.constData());
        std::fflush(stdout);
    }

    QJsonObject rectangle(const QRect& value)
    {
        return {{"x", value.x()}, {"y", value.y()}, {"width", value.width()}, {"height", value.height()}};
    }

    class Probe final : public QWidget
    {
      public:
        Probe()
        {
            this->setWindowTitle("Window behavior");
            this->resize(460, 360);
            this->move(30, 30);
            auto* layout = new QVBoxLayout(this);
            this->status_ = new QLabel(this);
            layout->addWidget(this->status_);
            this->editor_ = new QLineEdit(this);
            this->editor_->setPlaceholderText("Keyboard input");
            layout->addWidget(this->editor_);
            connect(this->editor_, &QLineEdit::textChanged, this, [this] { this->report(); });

            auto* menu_button = this->button(layout, "menu", "Menu");
            this->menu_ = new QMenu(this);
            auto* increment = this->menu_->addAction("Increment");
            this->menu_->addAction("Dismiss");
            connect(increment, &QAction::triggered, this, [this] {
                ++this->menu_actions_;
                this->report();
            });
            connect(this->menu_, &QMenu::aboutToShow, this, [this] {
                this->menu_visible_ = true;
                this->report();
                QTimer::singleShot(0, this, [this] { this->report_layout(); });
            });
            connect(this->menu_, &QMenu::aboutToHide, this, [this] {
                this->menu_visible_ = false;
                this->report();
            });
            connect(menu_button, &QPushButton::clicked, this,
                    [this, menu_button] { this->menu_->popup(menu_button->mapToGlobal(QPoint(0, menu_button->height()))); });
            connect(this->button(layout, "modal", "Open modal dialog"), &QPushButton::clicked, this, [this] { this->open_dialog(); });
            connect(this->button(layout, "tip", "Show tooltip"), &QPushButton::clicked, this, [this] { this->show_tooltip(); });
            connect(this->button(layout, "hide_tip", "Hide tooltip"), &QPushButton::clicked, this, [this] { this->hide_tooltip(); });
            this->dismiss_tip_ = new QShortcut(QKeySequence(Qt::Key_Escape), this);
            this->dismiss_tip_->setContext(Qt::ApplicationShortcut);
            this->dismiss_tip_->setEnabled(false);
            connect(this->dismiss_tip_, &QShortcut::activated, this, [this] { this->hide_tooltip(); });
            connect(this->button(layout, "count", "Count owner click"), &QPushButton::clicked, this, [this] {
                ++this->owner_clicks_;
                this->report();
            });
            auto* shortcut = new QShortcut(QKeySequence(Qt::Key_F6), this);
            connect(shortcut, &QShortcut::activated, this, [this] {
                ++this->shortcuts_;
                this->report();
            });
            this->editor_->setFocus();
            this->report();
            QTimer::singleShot(0, this, [this] { this->report_layout(); });
        }

      private:
        QPushButton* button(QVBoxLayout* layout, const char* name, const char* label)
        {
            auto* result = new QPushButton(QString::fromUtf8(label), this);
            result->setObjectName(QString::fromUtf8(name));
            layout->addWidget(result);
            return result;
        }

        void report()
        {
            this->status_->setText(QString("Menu %1 | Dialog %2 | Owner %3 | F6 %4")
                                       .arg(this->menu_actions_)
                                       .arg(this->dialog_actions_)
                                       .arg(this->owner_clicks_)
                                       .arg(this->shortcuts_));
            emit_json("WINDOW_STATE", {{"menu_actions", this->menu_actions_},
                                       {"dialog_actions", this->dialog_actions_},
                                       {"owner_clicks", this->owner_clicks_},
                                       {"shortcuts", this->shortcuts_},
                                       {"text", this->editor_->text()},
                                       {"menu_visible", this->menu_visible_},
                                       {"dialog_visible", this->dialog_ && this->dialog_->isVisible()},
                                       {"tip_visible", this->tip_visible_}});
        }

        void report_layout()
        {
            QJsonArray controls;
            for (auto* control : this->findChildren<QPushButton*>(QString(), Qt::FindDirectChildrenOnly))
            {
                auto item = rectangle(QRect(control->mapTo(this, QPoint()), control->size()));
                item.insert("name", control->objectName());
                controls.append(item);
            }
            auto editor = rectangle(QRect(this->editor_->mapTo(this, QPoint()), this->editor_->size()));
            QJsonObject data{{"window", rectangle(this->windowHandle()->geometry())}, {"controls", controls}, {"editor", editor}};
            if (this->menu_->isVisible())
            {
                data.insert("menu", rectangle(this->menu_->geometry()));
                data.insert("menu_action", rectangle(this->menu_->actionGeometry(this->menu_->actions().front())));
            }
            if (this->dialog_ && this->dialog_->isVisible())
            {
                data.insert("dialog", rectangle(this->dialog_->windowHandle()->geometry()));
                QJsonArray buttons;
                for (auto* control : this->dialog_->findChildren<QPushButton*>())
                {
                    auto item = rectangle(QRect(control->mapTo(this->dialog_, QPoint()), control->size()));
                    item.insert("name", control->objectName());
                    buttons.append(item);
                }
                data.insert("dialog_controls", buttons);
            }
            if (this->tooltip_ && this->tooltip_->isVisible())
            {
                data.insert("tooltip", rectangle(this->tooltip_->geometry()));
            }
            emit_json("WINDOW_LAYOUT", data);
        }

        void open_dialog()
        {
            if (this->dialog_)
            {
                return;
            }
            this->dialog_ = new QDialog(this);
            this->dialog_->setAttribute(Qt::WA_DeleteOnClose);
            this->dialog_->setWindowTitle("Modal check");
            this->dialog_->setWindowModality(Qt::WindowModal);
            this->dialog_->resize(260, 160);
            this->dialog_->move(this->mapToGlobal(QPoint(70, 70)));
            auto* layout = new QVBoxLayout(this->dialog_);
            layout->addWidget(new QLabel("The owner is blocked while this dialog is open.", this->dialog_));
            auto* action = new QPushButton("Dialog action", this->dialog_);
            action->setObjectName("action");
            layout->addWidget(action);
            connect(action, &QPushButton::clicked, this, [this] {
                ++this->dialog_actions_;
                this->report();
            });
            auto* close = new QPushButton("Close dialog", this->dialog_);
            close->setObjectName("close");
            layout->addWidget(close);
            connect(close, &QPushButton::clicked, this->dialog_, &QDialog::accept);
            connect(this->dialog_, &QDialog::finished, this, [this] { this->report(); });
            this->dialog_->show();
            action->setFocus();
            this->report();
            QTimer::singleShot(0, this, [this] { this->report_layout(); });
        }

        void hide_tooltip()
        {
            if (this->tooltip_)
            {
                this->tooltip_->hide();
            }
            this->tip_visible_ = false;
            this->dismiss_tip_->setEnabled(false);
            this->report();
        }

        void show_tooltip()
        {
            if (!this->tooltip_)
            {
                this->tooltip_ = new QLabel("Tooltip window", this, Qt::ToolTip);
                this->tooltip_->setStyleSheet("QLabel { background: #ffffc0; color: #101010; border: 1px solid "
                                              "#606060; padding: 4px; }");
                this->tooltip_->adjustSize();
            }
            this->tooltip_->move(this->mapToGlobal(QPoint(16, this->height() - this->tooltip_->height() - 12)));
            this->tooltip_->show();
            this->tip_visible_ = true;
            this->dismiss_tip_->setEnabled(true);
            this->report();
            QTimer::singleShot(0, this, [this] { this->report_layout(); });
        }

        QLabel* status_{};
        QLineEdit* editor_{};
        QMenu* menu_{};
        QShortcut* dismiss_tip_{};
        QPointer<QDialog> dialog_{};
        QPointer<QLabel> tooltip_{};
        int menu_actions_{};
        int dialog_actions_{};
        int owner_clicks_{};
        int shortcuts_{};
        bool menu_visible_{};
        bool tip_visible_{};
    };
} // namespace

int main(int argc, char** argv)
{
    QApplication application(argc, argv);
    Probe probe;
    probe.show();
    const auto result = application.exec();
    std::printf("APP_EXIT %d\n", result);
    std::fflush(stdout);
    return result;
}
