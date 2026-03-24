#pragma once

#include <QObject>
#include <QString>
#include <QVector>

namespace AetherSDR {

class CwxModel : public QObject {
    Q_OBJECT
public:
    explicit CwxModel(QObject* parent = nullptr);

    // State
    int   speed()    const { return m_speed; }
    int   delay()    const { return m_delay; }
    bool  qskOn()    const { return m_qsk; }
    bool  isLive()   const { return m_live; }
    int   sentIndex() const { return m_sentIndex; }
    QString macro(int idx) const;  // 0-based (0=F1, 11=F12)

    // Actions
    void send(const QString& text);      // Send mode: full string
    void sendChar(const QString& ch);    // Live mode: single char
    void sendMacro(int idx);             // 1-based (1=F1, 12=F12)
    void saveMacro(int idx, const QString& text); // 0-based
    void erase(int numChars);
    void clearBuffer();
    void setSpeed(int wpm);
    void setDelay(int ms);
    void setQsk(bool on);
    void setLive(bool on);

    // Status parsing (from radio)
    void applyStatus(const QMap<QString, QString>& kvs);

signals:
    void commandReady(const QString& cmd);
    void speedChanged(int wpm);
    void delayChanged(int ms);
    void qskChanged(bool on);
    void charSent(int index);           // character at index was keyed
    void erased(int start, int stop);
    void macroChanged(int idx, const QString& text);  // 0-based
    void liveChanged(bool on);

private:
    int     m_speed{20};
    int     m_delay{5};
    bool    m_qsk{false};
    bool    m_live{false};
    int     m_sentIndex{-1};
    int     m_nextBlock{1};
    QString m_macros[12];
};

} // namespace AetherSDR
