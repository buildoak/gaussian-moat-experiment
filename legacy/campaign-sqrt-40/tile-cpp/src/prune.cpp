#include "prune.h"

#include "constants.h"

#include <cstring>

namespace {

inline int count_faces(uint8_t mask) {
    int count = 0;
    while (mask != 0) {
        count += static_cast<int>(mask & 1U);
        mask = static_cast<uint8_t>(mask >> 1);
    }
    return count;
}

}  // namespace

FaceData prune_dead_ends(const FaceData& face_data) {
    FaceData pruned;
    std::memset(&pruned, 0, sizeof(pruned));

    uint8_t group_faces[MAX_PORTS + 1];
    uint16_t group_ports[MAX_PORTS + 1];
    std::memset(group_faces, 0, sizeof(group_faces));
    std::memset(group_ports, 0, sizeof(group_ports));

    for (int i = 0; i < face_data.port_count; ++i) {
        const Port& port = face_data.ports[i];
        if (port.group <= 0 || port.group > MAX_PORTS) {
            continue;
        }
        group_faces[port.group] = static_cast<uint8_t>(group_faces[port.group] | (1U << port.face));
        ++group_ports[port.group];
    }

    uint8_t dead_end[MAX_PORTS + 1];
    int remap[MAX_PORTS + 1];
    std::memset(dead_end, 0, sizeof(dead_end));
    std::memset(remap, 0, sizeof(remap));

    for (int group = 1; group <= face_data.group_count && group <= MAX_PORTS; ++group) {
        if (group_ports[group] == 1 && count_faces(group_faces[group]) == 1) {
            dead_end[group] = 1;
        }
    }

    int next_group = 1;
    for (int i = 0; i < face_data.port_count && pruned.port_count < MAX_PORTS; ++i) {
        const Port& port = face_data.ports[i];
        if (port.group <= 0 || port.group > MAX_PORTS || dead_end[port.group] != 0) {
            continue;
        }

        if (remap[port.group] == 0) {
            remap[port.group] = next_group++;
        }

        Port& out = pruned.ports[pruned.port_count++];
        out = port;
        out.group = remap[port.group];
    }

    pruned.group_count = next_group - 1;
    return pruned;
}
