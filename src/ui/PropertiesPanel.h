#pragma once

#include <QWidget>
#include "../core/PageInfo.h"

class QLabel;
class QFormLayout;

namespace pdfe::ui {

class PropertiesPanel : public QWidget {
    Q_OBJECT

public:
    explicit PropertiesPanel(QWidget* parent = nullptr);
    ~PropertiesPanel() override;

    void setPageInfo(const pdfe::core::PageInfo& info);
    void clearPageInfo();

private:
    QFormLayout* formLayout_ = nullptr;
    QLabel* pageIndexLabel_ = nullptr;
    QLabel* pageLabelLabel_ = nullptr;
    QLabel* widthLabel_ = nullptr;
    QLabel* heightLabel_ = nullptr;
    QLabel* rotationLabel_ = nullptr;
};

} // namespace pdfe::ui
