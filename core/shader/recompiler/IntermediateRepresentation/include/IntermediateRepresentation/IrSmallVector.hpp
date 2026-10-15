#ifndef CORE_SHADER_RECOMPILIER_INTERMEDIATEREPRESENTATION_INCLUDE_INTERMEDIATEREPRESENTATION_IRSMALLVECTOR_HPP
#define CORE_SHADER_RECOMPILIER_INTERMEDIATEREPRESENTATION_INCLUDE_INTERMEDIATEREPRESENTATION_IRSMALLVECTOR_HPP

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <iterator>
#include <memory>
#include <stdexcept>
#include <type_traits>
#include <vector>

namespace ShaderRecompiler {

template<typename TElement, std::size_t InlineCapacity>
class IrSmallVector {
    static_assert(std::is_trivially_copyable_v<TElement> && std::is_trivially_destructible_v<TElement>);

public:
    using value_type = TElement;
    using size_type = std::size_t;
    using iterator = TElement*;
    using const_iterator = const TElement*;

    IrSmallVector() = default;
    template<typename TIterator>
    IrSmallVector(TIterator from, TIterator to) { assign(from, to); }
    IrSmallVector(std::initializer_list<TElement> values) { assign(values.begin(), values.end()); }
    IrSmallVector(const IrSmallVector& other) { assign(other.begin(), other.end()); }
    IrSmallVector& operator=(const IrSmallVector& other) {
        if (this != &other) assign(other.begin(), other.end());
        return *this;
    }
    IrSmallVector(IrSmallVector&& other) noexcept { take(other); }
    IrSmallVector& operator=(IrSmallVector&& other) noexcept {
        if (this != &other) {
            release();
            take(other);
        }
        return *this;
    }
    IrSmallVector& operator=(const std::vector<TElement>& other) {
        assign(other.begin(), other.end());
        return *this;
    }
    ~IrSmallVector() { release(); }

    [[nodiscard]] size_type size() const { return count; }
    [[nodiscard]] bool empty() const { return count == 0; }
    [[nodiscard]] TElement* data() { return storage(); }
    [[nodiscard]] const TElement* data() const { return storage(); }
    [[nodiscard]] iterator begin() { return storage(); }
    [[nodiscard]] iterator end() { return storage() + count; }
    [[nodiscard]] const_iterator begin() const { return storage(); }
    [[nodiscard]] const_iterator end() const { return storage() + count; }
    [[nodiscard]] const_iterator cbegin() const { return begin(); }
    [[nodiscard]] const_iterator cend() const { return end(); }
    [[nodiscard]] std::reverse_iterator<iterator> rbegin() { return std::reverse_iterator<iterator>(end()); }
    [[nodiscard]] std::reverse_iterator<iterator> rend() { return std::reverse_iterator<iterator>(begin()); }
    [[nodiscard]] std::reverse_iterator<const_iterator> rbegin() const { return std::reverse_iterator<const_iterator>(end()); }
    [[nodiscard]] std::reverse_iterator<const_iterator> rend() const { return std::reverse_iterator<const_iterator>(begin()); }
    [[nodiscard]] TElement& operator[](size_type index) { return storage()[index]; }
    [[nodiscard]] const TElement& operator[](size_type index) const { return storage()[index]; }
    [[nodiscard]] TElement& at(size_type index) {
        if (index >= count) throw std::out_of_range("IrSmallVector index is out of range");
        return storage()[index];
    }
    [[nodiscard]] const TElement& at(size_type index) const {
        if (index >= count) throw std::out_of_range("IrSmallVector index is out of range");
        return storage()[index];
    }
    [[nodiscard]] TElement& front() { return storage()[0]; }
    [[nodiscard]] const TElement& front() const { return storage()[0]; }
    [[nodiscard]] TElement& back() { return storage()[count - 1]; }
    [[nodiscard]] const TElement& back() const { return storage()[count - 1]; }

    void push_back(const TElement& value) {
        if (count == capacity) grow(capacity * 2);
        storage()[count++] = value;
    }
    template<typename... TArguments>
    TElement& emplace_back(TArguments&&... arguments) {
        push_back(TElement{std::forward<TArguments>(arguments)...});
        return back();
    }
    void pop_back() { --count; }
    void clear() { count = 0; }
    void reserve(size_type wanted) {
        if (wanted > capacity) grow(std::max<size_type>(wanted, static_cast<size_type>(capacity) * 2u));
    }
    iterator erase(const_iterator position) {
        auto* first = storage();
        const auto index = static_cast<size_type>(position - first);
        std::copy(first + index + 1, first + count, first + index);
        --count;
        return first + index;
    }
    iterator erase(const_iterator from, const_iterator to) {
        auto* first = storage();
        const auto index = static_cast<size_type>(from - first);
        const auto last = static_cast<size_type>(to - first);
        std::copy(first + last, first + count, first + index);
        count -= last - index;
        return first + index;
    }
    template<typename TIterator>
    iterator insert(const_iterator position, TIterator from, TIterator to) {
        const auto index = static_cast<size_type>(position - storage());
        const auto added = static_cast<size_type>(std::distance(from, to));
        reserve(count + added);
        auto* first = storage();
        std::copy_backward(first + index, first + count, first + count + added);
        std::copy(from, to, first + index);
        count += static_cast<std::uint32_t>(added);
        return first + index;
    }
    iterator insert(const_iterator position, std::initializer_list<TElement> values) { return insert(position, values.begin(), values.end()); }
    template<typename TIterator>
    void assign(TIterator from, TIterator to) {
        const auto wanted = static_cast<size_type>(std::distance(from, to));
        count = 0;
        reserve(wanted);
        std::copy(from, to, storage());
        count = wanted;
    }
    [[nodiscard]] std::vector<TElement> ToVector() const { return std::vector<TElement>(begin(), end()); }
    operator std::vector<TElement>() const { return ToVector(); }
    [[nodiscard]] bool operator==(const IrSmallVector& other) const { return std::equal(begin(), end(), other.begin(), other.end()); }
    [[nodiscard]] bool operator<(const IrSmallVector& other) const { return std::lexicographical_compare(begin(), end(), other.begin(), other.end()); }
    [[nodiscard]] bool operator==(const std::vector<TElement>& other) const { return std::equal(begin(), end(), other.begin(), other.end()); }

private:
    TElement* storage() { return heap != nullptr ? heap : reinterpret_cast<TElement*>(buffer); }
    const TElement* storage() const { return heap != nullptr ? heap : reinterpret_cast<const TElement*>(buffer); }
    void grow(size_type wanted) {
        auto* grown = static_cast<TElement*>(::operator new(wanted * sizeof(TElement)));
        std::copy(storage(), storage() + count, grown);
        if (heap != nullptr) ::operator delete(heap);
        heap = grown;
        capacity = wanted;
    }
    void release() {
        if (heap != nullptr) ::operator delete(heap);
        heap = nullptr;
        capacity = InlineCapacity;
        count = 0;
    }
    void take(IrSmallVector& other) {
        if (other.heap != nullptr) {
            heap = other.heap;
            capacity = other.capacity;
            count = other.count;
            other.heap = nullptr;
            other.capacity = InlineCapacity;
            other.count = 0;
            return;
        }
        heap = nullptr;
        capacity = InlineCapacity;
        count = other.count;
        std::copy(other.begin(), other.end(), reinterpret_cast<TElement*>(buffer));
        other.count = 0;
    }

    TElement* heap = nullptr;
    std::uint32_t count = 0;
    std::uint32_t capacity = InlineCapacity;
    alignas(TElement) unsigned char buffer[InlineCapacity * sizeof(TElement)];
};

}

#endif
