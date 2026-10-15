#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

// A Bink byte stream backed by audio in ru_voice and video in the installed main OBB.
class Bl2BinkAudio {
public:
    bool Load(int audio_fd, uint64_t audio_base, uint64_t audio_bytes,
              int main_fd, uint64_t main_base, uint64_t main_bytes);
    int64_t Read(void* output, size_t bytes, int64_t offset) const;
    uint64_t Size() const { return bytes_; }
    void SetAudioFd(int fd) { audio_fd_ = fd; }

private:
    struct Span { uint32_t begin, bytes, offset, video; };
    std::vector<Span> spans_;
    uint64_t bytes_ = 0, audio_base_ = 0, video_base_ = 0;
    int audio_fd_ = -1, main_fd_ = -1;
};
