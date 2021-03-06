#pragma once

#include <bits/stringfwd.h>
#include <bits/memoryfwd.h>

namespace std {
    template<typename Sig>
    class function;

    template<typename T, typename A>
    class vector;

    class exception;
}

namespace bus {
    using GenericBuffer = std::vector<char, std::allocator<char>>;
}
