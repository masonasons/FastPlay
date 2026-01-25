#pragma once
#ifndef FASTPLAY_CYCLELIST_H
#define FASTPLAY_CYCLELIST_H

#include <vector>
#include <string>
#include <functional>

// Item in a CycleList
template<typename T>
struct CycleItem {
    T value;
    std::string label;
    int ctrlId;
    bool enabled;
    std::function<bool()> isAvailable;

    CycleItem(T val, const char* lbl, int id, bool defaultEnabled = false)
        : value(val), label(lbl), ctrlId(id), enabled(defaultEnabled),
          isAvailable([]() { return true; }) {}
};

// Reusable cycling list for navigation and effects
template<typename T>
class CycleList {
public:
    using ActionCallback = std::function<void(const T& value, int direction)>;
    using SpeakCallback = std::function<void(const std::string& text)>;

    CycleList(std::vector<CycleItem<T>> items, ActionCallback action, SpeakCallback speak)
        : m_items(std::move(items)), m_currentIndex(0), m_action(action), m_speak(speak) {
        ValidateCurrentIndex();
    }

    // Cycle through enabled items (direction: -1 or +1)
    bool Cycle(int direction) {
        int availableCount = GetAvailableCount();

        if (availableCount == 0) {
            m_speak("No items available");
            return false;
        }

        if (availableCount == 1) {
            ValidateCurrentIndex();
            AnnounceCurrentSelection();
            return false;
        }

        ValidateCurrentIndex();

        int newIndex = FindNextAvailable(direction);
        if (newIndex == m_currentIndex) {
            AnnounceCurrentSelection();
            return false;
        }

        m_currentIndex = newIndex;
        AnnounceCurrentSelection();
        return true;
    }

    // Apply current selection (direction: -1 for decrease, +1 for increase)
    void Apply(int direction) {
        if (m_currentIndex >= 0 && m_currentIndex < static_cast<int>(m_items.size())) {
            if (IsAvailable(m_currentIndex)) {
                m_action(m_items[m_currentIndex].value, direction);
            }
        }
    }

    // Get current item value
    const T& GetCurrentValue() const {
        return m_items[m_currentIndex].value;
    }

    // Get current item label
    const std::string& GetCurrentLabel() const {
        return m_items[m_currentIndex].label;
    }

    // Get current index
    int GetCurrentIndex() const {
        return m_currentIndex;
    }

    // Set current index
    void SetCurrentIndex(int index) {
        if (index >= 0 && index < static_cast<int>(m_items.size())) {
            m_currentIndex = index;
            ValidateCurrentIndex();
        }
    }

    // Enable/disable item by index
    void SetEnabled(int index, bool enabled) {
        if (index >= 0 && index < static_cast<int>(m_items.size())) {
            m_items[index].enabled = enabled;
        }
    }

    // Check if item at index is enabled
    bool IsEnabled(int index) const {
        if (index >= 0 && index < static_cast<int>(m_items.size())) {
            return m_items[index].enabled;
        }
        return false;
    }

    // Check if item is available (enabled AND passes availability check)
    bool IsAvailable(int index) const {
        if (index < 0 || index >= static_cast<int>(m_items.size())) {
            return false;
        }
        return m_items[index].enabled && m_items[index].isAvailable();
    }

    // Count available items
    int GetAvailableCount() const {
        int count = 0;
        for (size_t i = 0; i < m_items.size(); i++) {
            if (IsAvailable(static_cast<int>(i))) {
                count++;
            }
        }
        return count;
    }

    // Get all items (for dialog population)
    std::vector<CycleItem<T>>& GetItems() {
        return m_items;
    }

    const std::vector<CycleItem<T>>& GetItems() const {
        return m_items;
    }

    // Get item count
    int GetItemCount() const {
        return static_cast<int>(m_items.size());
    }

    // Announce current selection via screen reader
    void AnnounceCurrentSelection() const {
        if (m_currentIndex >= 0 && m_currentIndex < static_cast<int>(m_items.size())) {
            m_speak(m_items[m_currentIndex].label);
        }
    }

    // Set availability check for an item
    void SetAvailabilityCheck(int index, std::function<bool()> check) {
        if (index >= 0 && index < static_cast<int>(m_items.size())) {
            m_items[index].isAvailable = check;
        }
    }

private:
    std::vector<CycleItem<T>> m_items;
    int m_currentIndex;
    ActionCallback m_action;
    SpeakCallback m_speak;

    // Find next/previous available item (no wrap)
    int FindNextAvailable(int direction) const {
        int index = m_currentIndex;
        int count = static_cast<int>(m_items.size());

        for (int i = 0; i < count; i++) {
            index += direction;
            if (index < 0 || index >= count) {
                return m_currentIndex; // At boundary
            }
            if (IsAvailable(index)) {
                return index;
            }
        }
        return m_currentIndex;
    }

    // Ensure current index points to an available item
    void ValidateCurrentIndex() {
        if (IsAvailable(m_currentIndex)) {
            return;
        }

        // Find first available
        for (size_t i = 0; i < m_items.size(); i++) {
            if (IsAvailable(static_cast<int>(i))) {
                m_currentIndex = static_cast<int>(i);
                return;
            }
        }

        // Nothing available, reset to 0
        m_currentIndex = 0;
    }
};

#endif // FASTPLAY_CYCLELIST_H
