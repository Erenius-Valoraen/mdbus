#include <iostream>
#include <thread>

int main() {

    int num = 0;

    std::thread a([&] {
        num = 10;
    });

    std::thread b([&] {
        num = 20;
    });

    std::thread c([&] {
        for (int i = 0; i < 20; i++) {
            std::cout << num << "\n";
        }
    });


    a.join();
    b.join();
    c.join();

}