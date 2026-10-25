#include "hotkeys.hpp"
#include "contrast.hpp"
#include "i18n.hpp"
#include "settings.hpp"

#include <obs-module.h>

#include <QFocusEvent>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QPushButton>
#include <QSizePolicy>
#include <QVBoxLayout>

#include <cstdlib>

namespace livedelay {
namespace {

struct HotkeyDef {
	const char *name;
	const char *locale; // fixed actions; slots use Hotkey.ArmSlot
	const char *action;
	int slot; // -1 = not a delay slot
	obs_hotkey_id id = OBS_INVALID_HOTKEY_ID;
};

HotkeyDef g_keys[] = {
	{"livedelay.arm", "Hotkey.Arm", "arm", -1},
	{"livedelay.activate", "Hotkey.Activate", "activate", -1},
	{"livedelay.cut", "Hotkey.Cut", "cut", -1},
	{"livedelay.cut_after", "Hotkey.CutAfter", "cut-after", -1},
	{"livedelay.arm_slot.0", nullptr, "arm-slot", 0},
	{"livedelay.arm_slot.1", nullptr, "arm-slot", 1},
	{"livedelay.arm_slot.2", nullptr, "arm-slot", 2},
	{"livedelay.arm_slot.3", nullptr, "arm-slot", 3},
	{"livedelay.arm_slot.4", nullptr, "arm-slot", 4},
	{"livedelay.arm_slot.5", nullptr, "arm-slot", 5},
};

int g_seconds[kArmSlotCount] = {15, 30, 45, 60, 90, 120};
std::string g_slot_desc[kArmSlotCount];
void (*g_dispatch)(const std::string &) = nullptr;

QString ComboText(obs_key_combination_t combo)
{
	if (obs_key_combination_is_empty(combo))
		return T("Hotkey.None");
	QStringList parts;
	if (combo.modifiers & INTERACT_CONTROL_KEY)
		parts << "Ctrl";
	if (combo.modifiers & INTERACT_ALT_KEY)
		parts << "Alt";
	if (combo.modifiers & INTERACT_SHIFT_KEY)
		parts << "Shift";
	if (combo.modifiers & INTERACT_COMMAND_KEY)
		parts << "Win";
	QString key = QString::fromUtf8(obs_key_to_name(combo.key));
	if (key.startsWith("OBS_KEY_"))
		key = key.mid(8);
	parts << key;
	return parts.join("+");
}

obs_key_combination_t ReadCombo(obs_hotkey_id id)
{
	obs_key_combination_t combo = {0, OBS_KEY_NONE};
	obs_data_array_t *saved = obs_hotkey_save(id);
	if (saved && obs_data_array_count(saved) > 0) {
		obs_data_t *item = obs_data_array_item(saved, 0);
		combo.key = obs_key_from_name(obs_data_get_string(item, "key"));
		if (obs_data_get_bool(item, "shift"))
			combo.modifiers |= INTERACT_SHIFT_KEY;
		if (obs_data_get_bool(item, "control"))
			combo.modifiers |= INTERACT_CONTROL_KEY;
		if (obs_data_get_bool(item, "alt"))
			combo.modifiers |= INTERACT_ALT_KEY;
		if (obs_data_get_bool(item, "command"))
			combo.modifiers |= INTERACT_COMMAND_KEY;
		obs_data_release(item);
	}
	obs_data_array_release(saved);
	return combo;
}

void WriteCombo(obs_hotkey_id id, obs_key_combination_t combo)
{
	if (obs_key_combination_is_empty(combo)) {
		obs_hotkey_load_bindings(id, nullptr, 0);
		return;
	}
	obs_hotkey_load_bindings(id, &combo, 1);
}

void OnHotkey(void *data, obs_hotkey_id, obs_hotkey_t *, bool pressed)
{
	if (!pressed || !g_dispatch)
		return;
	const HotkeyDef *key = static_cast<HotkeyDef *>(data);
	if (key->slot >= 0) {
		const int sec = g_seconds[key->slot];
		if (sec < 1)
			return;
		g_dispatch("arm?delay=" + std::to_string(sec));
		return;
	}
	g_dispatch(key->action);
}

QString SlotDescription(int seconds)
{
	return T("Hotkey.ArmSlot").arg(seconds);
}

void RefreshSlotDescription(int slot)
{
	if (slot < 0 || slot >= kArmSlotCount)
		return;
	g_slot_desc[slot] = SlotDescription(g_seconds[slot]).toStdString();
	for (HotkeyDef &key : g_keys) {
		if (key.slot == slot && key.id != OBS_INVALID_HOTKEY_ID)
			obs_hotkey_set_description(key.id, g_slot_desc[slot].c_str());
	}
}

} // namespace

ComboButton::ComboButton(QWidget *parent) : QPushButton(parent)
{
	setMinimumWidth(168);
	setFocusPolicy(Qt::StrongFocus);
	Show();
	connect(this, &QPushButton::clicked, this, [this] {
		listening_ = true;
		setText(T("Hotkey.Press"));
		setFocus();
	});
}

void ComboButton::SetCombo(obs_key_combination_t next)
{
	combo = next;
	listening_ = false;
	Show();
}

void ComboButton::Show()
{
	setText(listening_ ? T("Hotkey.Press") : ComboText(combo));
}

void ComboButton::keyPressEvent(QKeyEvent *event)
{
	if (!listening_) {
		QPushButton::keyPressEvent(event);
		return;
	}
	if (event->isAutoRepeat())
		return;
	if (event->key() == Qt::Key_Escape) {
		listening_ = false;
		Show();
		return;
	}
	const obs_key_t key = obs_key_from_virtual_key(event->nativeVirtualKey());
	if (key == OBS_KEY_NONE || key == OBS_KEY_SHIFT || key == OBS_KEY_CONTROL || key == OBS_KEY_ALT ||
	    key == OBS_KEY_META)
		return;
	uint32_t mods = 0;
	const Qt::KeyboardModifiers m = event->modifiers();
	if (m & Qt::ShiftModifier)
		mods |= INTERACT_SHIFT_KEY;
	if (m & Qt::ControlModifier)
		mods |= INTERACT_CONTROL_KEY;
	if (m & Qt::AltModifier)
		mods |= INTERACT_ALT_KEY;
	if (m & Qt::MetaModifier)
		mods |= INTERACT_COMMAND_KEY;
	combo = {mods, key};
	listening_ = false;
	Show();
	event->accept();
}

void ComboButton::focusOutEvent(QFocusEvent *event)
{
	listening_ = false;
	Show();
	QPushButton::focusOutEvent(event);
}

void RegisterHotkeys(void (*dispatch)(const std::string &action))
{
	g_dispatch = dispatch;
	const Settings settings = Settings::Load(ConfigPath());
	for (int i = 0; i < kArmSlotCount; ++i)
		g_seconds[i] = settings.arm_slot[i];

	static std::string fixed_desc[4];
	int fixed_i = 0;
	for (HotkeyDef &key : g_keys) {
		if (key.slot >= 0) {
			g_slot_desc[key.slot] = SlotDescription(g_seconds[key.slot]).toStdString();
			key.id = obs_hotkey_register_frontend(key.name, g_slot_desc[key.slot].c_str(), OnHotkey, &key);
		} else {
			fixed_desc[fixed_i] = T(key.locale).toStdString();
			key.id = obs_hotkey_register_frontend(key.name, fixed_desc[fixed_i].c_str(), OnHotkey, &key);
			++fixed_i;
		}
	}
}

void RefreshHotkeyDescriptions()
{
	for (HotkeyDef &key : g_keys) {
		if (key.id == OBS_INVALID_HOTKEY_ID)
			continue;
		if (key.slot >= 0) {
			RefreshSlotDescription(key.slot);
		} else if (key.locale) {
			const QByteArray text = T(key.locale).toUtf8();
			obs_hotkey_set_description(key.id, text.constData());
		}
	}
}

void UnregisterHotkeys()
{
	for (HotkeyDef &key : g_keys) {
		if (key.id != OBS_INVALID_HOTKEY_ID)
			obs_hotkey_unregister(key.id);
		key.id = OBS_INVALID_HOTKEY_ID;
	}
	g_dispatch = nullptr;
}

void SaveHotkeyData(obs_data_t *data)
{
	for (HotkeyDef &key : g_keys) {
		obs_data_array_t *saved = obs_hotkey_save(key.id);
		obs_data_set_array(data, key.name, saved);
		obs_data_array_release(saved);
	}
}

void LoadHotkeyData(obs_data_t *data)
{
	for (HotkeyDef &key : g_keys) {
		obs_data_array_t *saved = obs_data_get_array(data, key.name);
		obs_hotkey_load(key.id, saved);
		obs_data_array_release(saved);
	}
}

// Wide enough for the translated label (Wyczyść, Löschen, Clear, Сброс, …).
// A fixed pixel width clips the longer languages.
QPushButton *MakeClearButton()
{
	auto *clear = new QPushButton(T("Hotkey.Clear"));
	clear->ensurePolished();
	clear->setMinimumWidth(clear->sizeHint().width());
	clear->setSizePolicy(QSizePolicy::Minimum, QSizePolicy::Fixed);
	return clear;
}

int ArmSeconds(int slot)
{
	if (slot < 0 || slot >= kArmSlotCount)
		return 0;
	return g_seconds[slot];
}

void SetArmSeconds(int slot, int seconds)
{
	if (slot < 0 || slot >= kArmSlotCount)
		return;
	if (seconds < 1)
		seconds = 1;
	if (seconds > 3600)
		seconds = 3600;
	g_seconds[slot] = seconds;
	RefreshSlotDescription(slot);
}

QWidget *MakeHotkeyPage(std::vector<ComboButton *> &keys, std::vector<QSpinBox *> &spins)
{
	keys.clear();
	spins.clear();
	auto *page = new QWidget;
	auto *lay = new QVBoxLayout(page);
	lay->setContentsMargins(4, 4, 4, 4);
	lay->setSpacing(10);
	auto *about = new QLabel(Rich(T("Hotkey.About"), true));
	about->setWordWrap(true);
	about->setTextFormat(Qt::RichText);
	about->setStyleSheet(QStringLiteral("color: %1;").arg(ThemeColor(true)));
	lay->addWidget(about);

	auto *grid = new QGridLayout;
	grid->setHorizontalSpacing(8);
	grid->setVerticalSpacing(6);
	grid->setColumnStretch(1, 1);
	int row = 0;
	auto header = [&](const QString &text) {
		auto *label = new QLabel(text);
		label->setStyleSheet("font-weight: 600; padding-top: 8px;");
		grid->addWidget(label, row, 0, 1, 3);
		++row;
	};
	auto addKey = [&](const QString &label, obs_hotkey_id id) {
		auto *name = new QLabel(label);
		auto *button = new ComboButton;
		button->SetCombo(ReadCombo(id));
		auto *clear = MakeClearButton();
		QObject::connect(clear, &QPushButton::clicked, button, [button] { button->SetCombo({0, OBS_KEY_NONE}); });
		grid->addWidget(name, row, 0);
		grid->addWidget(button, row, 1);
		grid->addWidget(clear, row, 2);
		keys.push_back(button);
		++row;
	};

	header(T("Hotkey.SectionActions"));
	for (HotkeyDef &key : g_keys) {
		if (key.slot >= 0)
			continue;
		addKey(T(key.locale), key.id);
	}

	header(T("Hotkey.SectionDelay"));
	auto *note = new QLabel(T("Hotkey.DelayNote"));
	note->setWordWrap(true);
	note->setStyleSheet(QStringLiteral("color: %1;").arg(ThemeColor(true)));
	grid->addWidget(note, row, 0, 1, 3);
	++row;
	for (HotkeyDef &key : g_keys) {
		if (key.slot < 0)
			continue;
		auto *spin = new QSpinBox;
		spin->setRange(1, 3600);
		spin->setSuffix(" s");
		spin->setValue(g_seconds[key.slot]);
		spin->setToolTip(T("Hotkey.SlotTip"));
		auto *button = new ComboButton;
		button->SetCombo(ReadCombo(key.id));
		auto *clear = MakeClearButton();
		QObject::connect(clear, &QPushButton::clicked, button, [button] { button->SetCombo({0, OBS_KEY_NONE}); });
		grid->addWidget(spin, row, 0);
		grid->addWidget(button, row, 1);
		grid->addWidget(clear, row, 2);
		keys.push_back(button);
		spins.push_back(spin);
		++row;
	}
	lay->addLayout(grid);
	lay->addStretch();
	return page;
}

void ApplyHotkeyPage(const std::vector<ComboButton *> &keys, const std::vector<QSpinBox *> &spins)
{
	size_t n = 0;
	for (HotkeyDef &key : g_keys) {
		if (n < keys.size() && keys[n])
			WriteCombo(key.id, keys[n]->combo);
		++n;
	}
	for (int i = 0; i < kArmSlotCount && i < int(spins.size()); ++i) {
		if (spins[i])
			SetArmSeconds(i, spins[i]->value());
	}
}

} // namespace livedelay
