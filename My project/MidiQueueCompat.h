#pragma once
#include <deque>
#include <vector>
#include <algorithm>
#include "IPlugMidi.h" // iplug::IMidiMsg

class MidiQueueCompat
{
public:
    void Clear() { mBuf.clear(); }
    bool Empty()            const { return mBuf.empty(); }
    int  ToDo()             const { return (int)mBuf.size(); }

    // добавляем без сортировки (обычно хост шлёт по возрастанию mOffset)
    void Add(const iplug::IMidiMsg& msg)
    {
        iplug::IMidiMsg m = msg;
        if (m.mOffset < 0) m.mOffset = 0;
        mBuf.push_back(m);                 // O(1)
    }
    // совместимость со старым кодом
    void Add(iplug::IMidiMsg* pMsg) { if (pMsg) Add(*pMsg); }

    // Забрать все события текущего блока: mOffset < nFrames.
    // Остальным сдвинуть offset на -nFrames (перенос в следующий блок).
    void GatherBlock(int nFrames, std::vector<iplug::IMidiMsg>& out)
    {
        out.clear();
        while (!mBuf.empty() && mBuf.front().mOffset < nFrames) {
            out.push_back(mBuf.front());     // O(1) pop_front
            mBuf.pop_front();
        }
        // На всякий случай отсортируем события этого блока по offset
        std::stable_sort(out.begin(), out.end(),
            [](const iplug::IMidiMsg& a, const iplug::IMidiMsg& b) { return a.mOffset < b.mOffset; });

        // События будущих блоков «подвигаем» разом
        for (auto& m : mBuf) m.mOffset -= nFrames;
    }

private:
    std::deque<iplug::IMidiMsg> mBuf; // pop_front()/push_back() = O(1)
};
