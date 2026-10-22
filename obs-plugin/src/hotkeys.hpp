#pragma once

#include <obs-module.h>
#include <obs-interaction.h>

#include <QPushButton>
#include <QSpinBox>
#include <QWidget>

#include <string>
#include <vector>

class QKeyEvent;
class QFocusEvent;

namespace livedelay {

constexpr int kArmSlotCount = 6;

// Click, then press a key. Escape leaves the current binding.
class ComboButton : public QPushButton {
	Q_OBJECT
public:
	explicit ComboButton(QWidget *parent = nullptr);
	obs_key_combination_t combo{0, OBS_KEY_NONE};
	void SetCombo(obs_key_combination_t next);

protected:
	void keyPressEvent(QKeyEvent *event) override;
	void focusOutEvent(QFocusEvent *event) override;

private:
	bool listening_ = false;
	void Show();
};

void RegisterHotkeys(void (*dispatch)(const std::string &action));
void RefreshHotkeyDescriptions();
void UnregisterHotkeys();
void SaveHotkeyData(obs_data_t *data);
void LoadHotkeyData(obs_data_t *data);

// Seconds for "arm this delay" slots. Updated when settings are saved.
int ArmSeconds(int slot);
void SetArmSeconds(int slot, int seconds);

QWidget *MakeHotkeyPage(std::vector<ComboButton *> &keys, std::vector<QSpinBox *> &spins);
void ApplyHotkeyPage(const std::vector<ComboButton *> &keys, const std::vector<QSpinBox *> &spins);

} // namespace livedelay
