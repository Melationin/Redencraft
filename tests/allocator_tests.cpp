#include "redstone/allocator.hpp"
#include <cstdint>
#include <iostream>
#include <memory>
#include <vector>
struct alignas(4096) Aligned { char data[8192]{}; };
int main() {
    auto p=std::make_unique<Aligned>();
    if(reinterpret_cast<std::uintptr_t>(p.get())%alignof(Aligned)) return 1;
    std::vector<std::unique_ptr<Aligned>> values;
    for(int i=0;i<128;i++) values.push_back(std::make_unique<Aligned>());
    void* zero=::operator new(0);
    if(!zero) return 2;
    ::operator delete(zero);
    std::cout << "allocator backend: " << redstone::memory::backend() << "\n";
}
