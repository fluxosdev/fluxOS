// What the native g++ compiles under QEMU (tests/gcc-guest.c): the parts of C++
// that need the runtime library: iostream, strings, containers, algorithms,
// exceptions thrown through functions, virtual calls, smart pointers, and
// static objects constructed before main and destroyed after it.
#include <algorithm>
#include <cstdio>
#include <iostream>
#include <map>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

struct Shape {
    virtual ~Shape() {}
    virtual double area() const = 0;
    virtual std::string name() const = 0;
};

struct Square : Shape {
    double s;
    explicit Square(double side) : s(side) {}
    double area() const override { return s * s; }
    std::string name() const override { return "square"; }
};

struct Circle : Shape {
    double r;
    explicit Circle(double radius) : r(radius) {}
    double area() const override { return 3.0 * r * r; }
    std::string name() const override { return "circle"; }
};

static struct Lifetime {
    Lifetime() { std::puts("static object constructed"); }
    ~Lifetime() { std::puts("static object destroyed"); }
} lifetime;

static int checked_divide(int a, int b)
{
    if (b == 0)
        throw std::invalid_argument("division by zero");
    return a / b;
}

static int deeper(int depth)
{
    if (depth == 0)
        return checked_divide(1, 0);
    return deeper(depth - 1) + 1;
}

int main()
{
    std::vector<std::string> words = { "pear", "apple", "fig", "kiwi" };
    std::sort(words.begin(), words.end());
    std::map<std::string, size_t> lengths;
    for (const auto &w : words)
        lengths[w] = w.size();

    std::vector<std::unique_ptr<Shape>> shapes;
    shapes.push_back(std::make_unique<Square>(3));
    shapes.push_back(std::make_unique<Circle>(2));
    double total = 0;
    for (const auto &s : shapes)
        total += s->area();

    std::string caught = "nothing";
    try {
        deeper(5);
    } catch (const std::exception &e) {
        caught = e.what();
    }

    std::ostringstream out;
    out << "hello from fluxOS g++:";
    for (const auto &w : words)
        out << ' ' << w;
    out << " | kiwi=" << lengths["kiwi"] << " | area=" << total << " | caught: " << caught;
    std::cout << out.str() << std::endl;
    return 0;
}
