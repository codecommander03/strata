// Stage 1 gate, the half a unit test cannot express: a real process, writing
// real transactions, killed by the operating system without warning.
//
//   strata_crash write   <db> <count> <batch>   append keys, committing batches
//   strata_crash verify  <db> [expected]        recover, check every invariant
//
// The writer never checkpoints, so everything it committed lives in the log and
// recovery is doing genuine work rather than reading an already-clean file.
// scripts/chaos.ps1 drives it.

#include "strata/btree.hpp"
#include "strata/pager.hpp"

#include <cstdio>
#include <cstdlib>
#include <string>

using namespace strata;

namespace {

std::string key_for(int i) {
    std::string s = std::to_string(i);
    return std::string(10 - s.size(), '0') + s;
}

int usage() {
    std::fprintf(stderr, "usage: strata_crash write <db> <count> <batch>\n"
                         "       strata_crash verify <db> [expected]\n");
    return 2;
}

int do_write(const std::string& path, int count, int batch) {
    Pager pager;
    if (Status s = pager.open(path); !s) {
        std::fprintf(stderr, "open failed: %s\n", s.to_string().c_str());
        return 1;
    }
    BTree tree(pager);

    for (int i = 0; i < count; ++i) {
        const std::string key = key_for(i);
        const std::string value = "value-" + std::to_string(i);
        if (Status s = tree.insert(as_bytes(key), as_bytes(value)); !s) {
            std::fprintf(stderr, "insert %d failed: %s\n", i, s.to_string().c_str());
            return 1;
        }

        if ((i + 1) % batch == 0) {
            if (Status s = pager.commit(); !s) {
                std::fprintf(stderr, "commit failed: %s\n", s.to_string().c_str());
                return 1;
            }
            // Announce progress only after the commit is durable, so whatever
            // the harness last read is guaranteed to be recoverable.
            std::printf("committed %d\n", i + 1);
            std::fflush(stdout);
        }
    }

    if (Status s = pager.commit(); !s) {
        std::fprintf(stderr, "final commit failed: %s\n", s.to_string().c_str());
        return 1;
    }
    std::printf("committed %d\ndone\n", count);
    std::fflush(stdout);
    return 0;
}

int do_verify(const std::string& path, long expected) {
    Pager pager;
    if (Status s = pager.open(path); !s) {
        std::fprintf(stderr, "RECOVERY FAILED: %s\n", s.to_string().c_str());
        return 1;
    }
    BTree tree(pager);

    std::size_t keys = 0;
    if (Status s = tree.verify_integrity(&keys); !s) {
        std::fprintf(stderr, "INTEGRITY FAILED: %s\n", s.to_string().c_str());
        return 1;
    }

    // Every key the tree claims to hold must also read back with the value it
    // was written with. An index that is internally consistent but returns the
    // wrong bytes is still a broken database.
    for (std::size_t i = 0; i < keys; ++i) {
        std::string value;
        const std::string key = key_for(static_cast<int>(i));
        if (Status s = tree.get(as_bytes(key), &value); !s) {
            std::fprintf(stderr, "HOLE: key %s missing but %zu keys present\n", key.c_str(), keys);
            return 1;
        }
        if (value != "value-" + std::to_string(i)) {
            std::fprintf(stderr, "WRONG VALUE for %s: %s\n", key.c_str(), value.c_str());
            return 1;
        }
    }

    std::printf("ok keys=%zu\n", keys);
    if (expected >= 0 && static_cast<long>(keys) < expected) {
        std::fprintf(stderr, "LOST DATA: expected at least %ld committed keys, found %zu\n",
                     expected, keys);
        return 1;
    }
    return 0;
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 3) {
        return usage();
    }
    const std::string mode = argv[1];
    const std::string path = argv[2];

    if (mode == "write") {
        if (argc < 5) {
            return usage();
        }
        return do_write(path, std::atoi(argv[3]), std::atoi(argv[4]));
    }
    if (mode == "verify") {
        const long expected = argc >= 4 ? std::atol(argv[3]) : -1;
        return do_verify(path, expected);
    }
    return usage();
}
