#include <QtWidgets>
#include <cmath>
#include <cstdio>

class CalculatorState
{
  public:
    QString text = "0", pending, repeated;
    double accumulator = 0, memory = 0, operand = 0;
    bool fresh = true, error = false, hasRepeated = false;

    double value() const
    {
        return text.toDouble();
    }

    void number(double n)
    {
        error = !std::isfinite(n);
        text = error ? "Error" : QString::number(n == 0 ? 0 : n, 'g', 14);
    }

    void clear()
    {
        text = "0";
        pending.clear();
        repeated.clear();
        accumulator = 0;
        fresh = true;
        error = false;
        hasRepeated = false;
    }

    double apply(double a, double b, const QString& op)
    {
        if (op == "+")
        {
            return a + b;
        }
        if (op == "-")
        {
            return a - b;
        }
        if (op == "*")
        {
            return a * b;
        }
        return b == 0 ? std::numeric_limits<double>::quiet_NaN() : a / b;
    }

    void press(const QString& key)
    {
        if (key == "C")
        {
            clear();
            return;
        }
        if (key == "MC")
        {
            memory = 0;
            return;
        }
        if (key == "MR")
        {
            number(memory);
            fresh = true;
            return;
        }
        if (error)
        {
            if (key.size() == 1 && (key[0].isDigit() || key == "."))
            {
                clear();
            }
            else
            {
                return;
            }
        }
        if (key == "M+")
        {
            const double n = memory + value();
            if (std::isfinite(n))
            {
                memory = n;
            }
            return;
        }
        if (key == "M-")
        {
            const double n = memory - value();
            if (std::isfinite(n))
            {
                memory = n;
            }
            return;
        }
        if (key == "CE")
        {
            text = "0";
            fresh = true;
            return;
        }
        if (key == "back")
        {
            if (!fresh)
            {
                text.chop(1);
                if (text.isEmpty() || text == "-")
                {
                    text = "0";
                }
            }
            return;
        }
        if (key == "sign")
        {
            number(-value());
            return;
        }
        if (key == "%")
        {
            number((pending == "+" || pending == "-") ? accumulator * value() / 100 : value() / 100);
            fresh = false;
            return;
        }
        if (key.size() == 1 && (key[0].isDigit() || key == "."))
        {
            if (fresh)
            {
                text = "0";
                fresh = false;
            }
            hasRepeated = false;
            if (key == ".")
            {
                if (!text.contains('.'))
                {
                    text += '.';
                }
            }
            else if (text == "0")
            {
                text = key;
            }
            else if (text.size() < 16)
            {
                text += key;
            }
            return;
        }
        if (key == "=")
        {
            if (!pending.isEmpty())
            {
                operand = value();
                repeated = pending;
                hasRepeated = true;
                number(apply(accumulator, operand, pending));
                pending.clear();
            }
            else if (hasRepeated)
            {
                number(apply(value(), operand, repeated));
            }
            fresh = true;
            return;
        }
        if (key == "+" || key == "-" || key == "*" || key == "/")
        {
            if (!pending.isEmpty() && !fresh)
            {
                number(apply(accumulator, value(), pending));
            }
            accumulator = value();
            pending = key;
            fresh = true;
            hasRepeated = false;
        }
    }
};

static void record(const QString& line)
{
    const auto bytes = (line + '\n').toUtf8();
    std::fwrite(bytes.constData(), 1, size_t(bytes.size()), stdout);
    std::fflush(stdout);
    QFile file(QCoreApplication::applicationDirPath() + "/application.log");
    if (file.open(QIODevice::WriteOnly | QIODevice::Append))
    {
        file.write(bytes);
    }
}

class Calculator : public QWidget
{
    bool probe_{};
    CalculatorState state;
    QLabel *display, *hint;

  public:
    explicit Calculator(bool probe = false)
        : probe_(probe)
    {
        setWindowTitle("Pocket Calculator");
        setObjectName("calculator");
        setMinimumSize(340, 500);
        resize(380, 550);
        auto palette = this->palette();
        palette.setColor(QPalette::Window, QColor("#f1f4f8"));
        palette.setColor(QPalette::WindowText, QColor("#17263c"));
        palette.setColor(QPalette::ButtonText, QColor("#17263c"));
        palette.setColor(QPalette::Button, QColor("#ffffff"));
        setPalette(palette);
        auto* layout = new QVBoxLayout(this);
        layout->setContentsMargins(18, 18, 18, 18);
        layout->setSpacing(10);
        auto* title = new QLabel("POCKET / CALCULATOR");
        QFont small("DejaVu Sans", 10);
        title->setFont(small);
        layout->addWidget(title);
        hint = new QLabel("Ready");
        hint->setFont(small);
        hint->setMinimumHeight(24);
        layout->addWidget(hint);
        display = new QLabel("0");
        display->setObjectName("display");
        display->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
        display->setFont(QFont("DejaVu Sans", 28));
        display->setMinimumHeight(70);
        layout->addWidget(display);
        auto* memory = new QHBoxLayout;
        memory->setSpacing(8);
        for (const auto& key : {"MC", "MR", "M+", "M-"})
        {
            memory->addWidget(button(key, key, false));
        }
        layout->addLayout(memory);
        auto* grid = new QGridLayout;
        grid->setSpacing(8);
        const QStringList keys = {"%", "CE", "C", "back", "7", "8", "9",    "/", "4", "5",
                                  "6", "*",  "1", "2",    "3", "-", "sign", "0", ".", "+"};
        const QMap<QString, QString> labels = {{"back", "⌫"}, {"/", "÷"}, {"*", "×"}, {"sign", "±"}, {"-", "−"}};
        for (int i = 0; i < keys.size(); ++i)
        {
            const auto& key = keys[i];
            auto* b = button(key, labels.value(key, key), key == "/" || key == "*" || key == "-" || key == "+");
            b->setMinimumHeight(48);
            grid->addWidget(b, i / 4, i % 4);
        }
        layout->addLayout(grid, 1);
        auto* equals = button("=", "=", true);
        equals->setMinimumHeight(50);
        layout->addWidget(equals);
        setFocusPolicy(Qt::StrongFocus);
        QTimer::singleShot(0, this, [this] {
            traceLayout();
            refresh("ready");
        });
    }

    QPushButton* button(const QString& key, const QString& label, bool accent)
    {
        auto* b = new QPushButton(label);
        b->setObjectName(key);
        b->setAccessibleName(key);
        b->setFont(QFont("DejaVu Sans", 15));
        b->setMinimumHeight(34);
        b->setFocusPolicy(Qt::NoFocus);
        if (accent)
        {
            auto p = palette();
            p.setColor(QPalette::Button, QColor("#c8e5fb"));
            b->setPalette(p);
        }
        connect(b, &QPushButton::clicked, this, [this, key] {
            state.press(key);
            refresh(key);
        });
        return b;
    }

    void refresh(const QString& key)
    {
        display->setText(state.text);
        display->setFont(QFont("DejaVu Sans", state.text.size() > 12 ? 18 : 28));
        hint->setText(state.error
                          ? "Cannot divide by zero or exceed range"
                          : (state.memory != 0 ? "M   " : "") +
                                (state.pending.isEmpty() ? "Ready" : QString::number(state.accumulator, 'g', 14) + " " + state.pending));
        record("CALC_STATE " +
               QString::fromUtf8(
                   QJsonDocument(QJsonObject{{"key", key}, {"display", state.text}, {"memory", state.memory}, {"error", state.error}})
                       .toJson(QJsonDocument::Compact)));
    }

    void traceLayout()
    {
        QJsonArray controls;
        for (auto* b : findChildren<QPushButton*>())
        {
            const auto p = b->mapTo(this, QPoint(0, 0));
            controls.append(
                QJsonObject{{"name", b->objectName()}, {"x", p.x()}, {"y", p.y()}, {"width", b->width()}, {"height", b->height()}});
        }
        const QJsonArray windows{QJsonObject{
            {"id", double(winId())}, {"title", windowTitle()}, {"width", width()}, {"height", height()}, {"controls", controls}}};
        record("APP_LAYOUT " + QString::fromUtf8(QJsonDocument(windows).toJson(QJsonDocument::Compact)));
    }

    void resizeEvent(QResizeEvent* event) override
    {
        QWidget::resizeEvent(event);
        QTimer::singleShot(0, this, [this] { traceLayout(); });
    }

    void keyPressEvent(QKeyEvent* event) override
    {
        QString key = event->text();
        if (event->key() == Qt::Key_Return || event->key() == Qt::Key_Enter)
        {
            key = "=";
        }
        else if (event->key() == Qt::Key_Escape)
        {
            key = "C";
        }
        else if (event->key() == Qt::Key_Backspace)
        {
            key = "back";
        }
        else if (event->key() == Qt::Key_Delete)
        {
            key = "CE";
        }
        else if (key == ",")
        {
            key = ".";
        }
        state.press(key);
        refresh(key);
        event->accept();
        if (probe_ && key == "=")
        {
            const int result = state.text == "5" ? 0 : 42;
            QTimer::singleShot(0, qApp, [result] { QCoreApplication::exit(result); });
        }
    }
};

static int selfTest()
{
    const QList<QPair<QStringList, QString>> cases = {{{"1", "2", "+", "3", "4", "="}, "46"},
                                                      {{"9", "-", "1", "2", "="}, "-3"},
                                                      {{"6", "*", "7", "="}, "42"},
                                                      {{"8", "/", "4", "="}, "2"},
                                                      {{"0", ".", "1", "+", "0", ".", "2", "="}, "0.3"},
                                                      {{"2", "0", "0", "+", "1", "0", "%", "="}, "220"},
                                                      {{"8", "/", "0", "="}, "Error"},
                                                      {{"8", "/", "0", "=", "7"}, "7"},
                                                      {{"5", "+", "2", "=", "="}, "9"},
                                                      {{"9", "M+", "C", "MR"}, "9"},
                                                      {{"5", "sign", "*", "2", "="}, "-10"},
                                                      {{"1", "2", "back"}, "1"},
                                                      {{"2", "+", "3", "*", "4", "="}, "20"},
                                                      {{"7", "+", "-", "2", "="}, "5"}};
    for (const auto& test : cases)
    {
        CalculatorState state;
        for (const auto& key : test.first)
        {
            state.press(key);
        }
        if (state.text != test.second)
        {
            std::fprintf(stderr, "FAIL %s != %s\n", qPrintable(state.text), qPrintable(test.second));
            return 1;
        }
    }
    std::printf("CALCULATOR_SELF_TEST_PASS %lld cases\n", static_cast<long long>(cases.size()));
    return 0;
}

int main(int argc, char** argv)
{
    for (int i = 1; i < argc; ++i)
    {
        if (QByteArray(argv[i]) == "--self-test")
        {
            return selfTest();
        }
    }
    QApplication app(argc, argv);
    QApplication::setStyle("Fusion");
    const bool probe = app.arguments().contains("--probe");
    Calculator calculator(probe);
    calculator.show();
    const int code = app.exec();
    record(QString("APP_EXIT %1").arg(code));
    return code;
}
