#pragma once

#include <cassert>
#include <exception>
#include <functional>
#include <type_traits>

namespace chibicpp {

template <typename T, typename E>
class Expected;

template <typename E>
class UnExpected;

template <typename E>
class bad_expected_access;

template <>
class bad_expected_access<void> : public std::exception {
 protected:
  bad_expected_access() noexcept {}
  bad_expected_access(const bad_expected_access&) = default;
  bad_expected_access(bad_expected_access&&) = default;
  bad_expected_access& operator=(const bad_expected_access&) = default;
  bad_expected_access& operator=(bad_expected_access&&) = default;
  ~bad_expected_access() = default;

 public:
  [[nodiscard]] const char* what() const noexcept override {
    return "bad access to std::expected without expected value";
  }
};

template <typename E>
class bad_expected_access : public bad_expected_access<void> {
 public:
  explicit bad_expected_access(E e) : unex_(std::move(e)) {}

  // XXX const char* what() const noexcept override;

  [[nodiscard]] E& error() & noexcept { return unex_; }

  [[nodiscard]] const E& error() const& noexcept { return unex_; }

  [[nodiscard]] E&& error() && noexcept { return std::move(unex_); }

  [[nodiscard]] const E&& error() const&& noexcept { return std::move(unex_); }

 private:
  E unex_;
};

struct unexpect_t {
  explicit unexpect_t() = default;
};

inline constexpr unexpect_t unexpect{};

namespace expected {

template <typename Tp>
constexpr bool is_expected = false;
template <typename Tp, typename E>
constexpr bool is_expected<Expected<Tp, E>> = true;

template <typename Tp>
constexpr bool is_unexpected = false;

template <typename Tp>
constexpr bool is_unexpected<UnExpected<Tp>> = true;

template <typename Fn, typename Tp>
using result = std::remove_cvref_t<std::invoke_result_t<Fn&&, Tp&&>>;
template <typename Fn, typename Tp>
using result_xform = std::remove_cv_t<std::invoke_result_t<Fn&&, Tp&&>>;
template <typename Fn>
using result0 = std::remove_cvref_t<std::invoke_result_t<Fn&&>>;
template <typename Fn>
using result0_xform = std::remove_cv_t<std::invoke_result_t<Fn&&>>;

template <typename E>
concept can_be_unexpected = std::is_object_v<E> && (!std::is_array_v<E>) &&
                            (!expected::is_unexpected<E>) &&
                            (!std::is_const_v<E>) && (!std::is_volatile_v<E>);

// Tag types for in-place construction from an invocation result.
struct in_place_inv {};
struct unexpect_inv {};

}  // namespace expected

/// The class template std::unexpected represents an unexpected value stored in
/// std::expected. In particular, std::expected has constructors with
/// std::unexpected as a single argument, which creates an expected object that
/// contains an unexpected value.
/// A program is ill-formed if it instantiates an unexpected with a non-object
/// type, an array type, a specialization of std::unexpected, or a cv-qualified
/// type.
template <typename E>
class UnExpected {
  static_assert(expected::can_be_unexpected<E>);

 public:
  constexpr UnExpected(const UnExpected&) = default;
  constexpr UnExpected(UnExpected&&) = default;

  /// Constructs the stored value, as if by direct-initializing a value of type
  /// E from std::forward<Err>(e).
  /// This overload participates in overload resolution only i
  /// - std::is_same_v<std::remove_cvref_t<Err>, unexpected> is false, and
  /// - std::is_same_v<std::remove_cvref_t<Err>, std::in_place_t> is false, and
  /// - std::is_constructible_v<E, Err> is true.
  template <typename Err = E>
    requires(!std::is_same_v<std::remove_cvref_t<Err>, UnExpected>) &&
            (!std::is_same_v<std::remove_cvref_t<Err>, std::in_place_t>) &&
            std::is_constructible_v<E, Err>
  constexpr explicit UnExpected(Err&& e) noexcept(
      std::is_nothrow_constructible_v<E, Err>)
      : unex_(std::forward<Err>(e)) {}

  /// Constructs the stored value, as if by direct-initializing a value of type
  /// E from the arguments std::forward<Args>(args)....
  /// - This overload participates in overload resolution only if
  ///   std::is_constructible_v<E, Args...> is true.
  template <typename... Args>
    requires std::is_constructible_v<E, Args...>
  constexpr explicit UnExpected(std::in_place_t, Args&&... args) noexcept(
      std::is_nothrow_constructible_v<E, Args...>)
      : unex_(std::forward<Args>(args)...) {}

  /// Constructs the stored value, as if by direct-initializing a value of type
  /// E from the arguments il, std::forward<Args>(args)....
  /// - This overload participates in overload resolution only if
  ///   std::is_constructible_v<E, std::initializer_list<U>&, Args...> is true.
  template <typename Up, typename... Args>
    requires std::is_constructible_v<E, std::initializer_list<Up>&, Args...>
  constexpr explicit UnExpected(
      std::in_place_t, std::initializer_list<Up> il,
      Args&&... args) noexcept(std::
                                   is_nothrow_constructible_v<
                                       E, std::initializer_list<Up>&, Args...>)
      : unex_(il, std::forward<Args>(args)...) {}

  constexpr UnExpected& operator=(const UnExpected&) = default;
  constexpr UnExpected& operator=(UnExpected&&) = default;

  [[nodiscard]] constexpr const E& Error() const& noexcept { return unex_; }
  [[nodiscard]] constexpr E& Error() & noexcept { return unex_; }
  [[nodiscard]] constexpr const E&& Error() const&& noexcept {
    return std::move(unex_);
  }
  [[nodiscard]] constexpr E&& Error() && noexcept { return std::move(unex_); }

  constexpr void swap(UnExpected& other) noexcept(
      std::is_nothrow_swappable_v<E>)
    requires std::is_swappable_v<E>
  {
    using std::swap;

    swap(unex_, other.unex_);
  }

  template <typename Err>
  [[nodiscard]] friend constexpr bool operator==(const UnExpected& x,
                                                 const UnExpected<Err>& y) {
    return x.unex_ == y.error();
  }

  friend constexpr void swap(UnExpected& x,
                             UnExpected& y) noexcept(noexcept(x.swap(y)))
    requires std::is_swappable_v<E>
  {
    x.swap(y);
  }

 private:
  E unex_;
};

template <typename E>
UnExpected(E) -> UnExpected<E>;

/// \cond undocumented
namespace expected {

template <typename T>
struct Guard {
  static_assert(std::is_nothrow_move_constructible_v<T>);

  constexpr explicit Guard(T& x)
      : guard_(std::addressof(x)), tmp_(std::move(x)) {
    std::destroy_at(guard_);
  }

  constexpr ~Guard() {
    if (guard_) [[unlikely]]
      std::construct_at(guard_, std::move(tmp_));
  }

  Guard(const Guard&) = delete;
  Guard& operator=(const Guard&) = delete;

  constexpr T&& Release() noexcept {
    guard_ = nullptr;

    return std::move(tmp_);
  }

 private:
  T* guard_;
  T tmp_;
};

// reinit-expected helper from [expected.object.assign]
template <typename T, typename U, typename V>
constexpr void reinit(T* newval, U* oldval,
                      V&& arg) noexcept(std::is_nothrow_constructible_v<T, V>) {
  if constexpr (std::is_nothrow_constructible_v<T, V>) {
    std::destroy_at(oldval);
    std::construct_at(newval, std::forward<V>(arg));
  } else if constexpr (std::is_nothrow_move_constructible_v<T>) {
    T tmp(std::forward<V>(arg));  // might throw
    std::destroy_at(oldval);
    std::construct_at(newval, std::move(tmp));
  } else {
    Guard<U> guard(*oldval);
    std::construct_at(newval, std::forward<V>(arg));  // might throw
    guard.release();
  }
}

}  // namespace expected
   /// \endcond

template <typename T, typename E>
class Expected {
  static_assert(!std::is_reference_v<T>);
  static_assert(!std::is_function_v<T>);
  static_assert(!std::is_same_v<std::remove_cv_t<T>, std::in_place_t>);
  static_assert(!std::is_same_v<std::remove_cv_t<T>, unexpect_t>);
  static_assert(!expected::is_unexpected<std::remove_cv_t<T>>);
  static_assert(expected::can_be_unexpected<E>);

  template <typename U, typename Err, typename Unex = UnExpected<E>>
  static constexpr bool cons_from_expected =
      std::disjunction_v<std::is_constructible<T, Expected<U, Err>&>,
                         std::is_constructible<T, Expected<U, Err>>,
                         std::is_constructible<T, const Expected<U, Err>&>,
                         std::is_constructible<T, const Expected<U, Err>>,
                         std::is_convertible<Expected<U, Err>&, T>,
                         std::is_convertible<Expected<U, Err>, T>,
                         std::is_convertible<const Expected<U, Err>&, T>,
                         std::is_convertible<const Expected<U, Err>, T>,
                         std::is_constructible<Unex, Expected<U, Err>&>,
                         std::is_constructible<Unex, Expected<U, Err>>,
                         std::is_constructible<Unex, const Expected<U, Err>&>,
                         std::is_constructible<Unex, const Expected<U, Err>>>;

  template <typename U, typename Err>
  constexpr static bool explicit_conv =
      std::disjunction_v<std::negation<std::is_convertible<U, T>>,
                         std::negation<std::is_convertible<Err, E>>>;

  template <typename U>
  static constexpr bool same_val = std::is_same_v<typename U::value_type, T>;

  template <typename U>
  static constexpr bool same_err = std::is_same_v<typename U::error_type, E>;

 public:
  using value_type = T;
  using error_type = E;
  using unexpected_type = UnExpected<E>;

  template <typename U>
  using rebind = Expected<U, error_type>;

  constexpr Expected() noexcept(std::is_nothrow_default_constructible_v<T>)
    requires std::is_default_constructible_v<T>
      : val_(), has_value_(true) {}

  Expected(const Expected&) = default;

  constexpr Expected(const Expected& x) noexcept(
      std::conjunction_v<std::is_nothrow_copy_constructible<T>,
                         std::is_nothrow_copy_constructible<E>>)
    requires std::is_copy_constructible_v<T> &&
             std::is_copy_constructible_v<E> &&
             (!std::is_trivially_copy_constructible_v<T> ||
              !std::is_trivially_copy_constructible_v<E>)
      : has_value_(x.has_value_) {
    if (has_value_) {
      std::construct_at(std::addressof(val_), x.val_);
    } else {
      std::construct_at(std::addressof(unex_), x.unex_);
    }
  }

  Expected(Expected&&) = default;

  constexpr Expected(Expected&& x) noexcept(
      std::conjunction_v<std::is_nothrow_move_constructible<T>,
                         std::is_nothrow_move_constructible<E>>)
    requires std::is_move_constructible_v<T> &&
             std::is_move_constructible_v<E> &&
             (!std::is_trivially_move_constructible_v<T> ||
              !std::is_trivially_move_constructible_v<E>)
      : has_value_(x.has_value_) {
    if (has_value_) {
      std::construct_at(std::addressof(val_), std::move(x).val_);
    } else {
      std::construct_at(std::addressof(unex_), std::move(x).unex_);
    }
  }

  template <typename U, typename Gr>
    requires std::is_constructible_v<T, const U&> &&
             std::is_constructible_v<E, const Gr&> &&
             (!cons_from_expected<U, Gr>)
  constexpr explicit(explicit_conv<const U&, const Gr&>)
      Expected(const Expected<U, Gr>& x) noexcept(
          std::conjunction_v<std::is_nothrow_constructible<T, const U&>,
                             std::is_nothrow_constructible<E, const Gr&>>)
      : has_value_(x.has_value_) {
    if (has_value_) {
      std::construct_at(std::addressof(val_), x.val_);
    } else {
      std::construct_at(std::addressof(unex_), x.unex_);
    }
  }

  template <typename U, typename Gr>
    requires std::is_constructible_v<T, U> && std::is_constructible_v<E, Gr> &&
             (!cons_from_expected<U, Gr>)
  constexpr explicit(explicit_conv<U, Gr>)
      Expected(Expected<U, Gr>&& x) noexcept(
          std::conjunction_v<std::is_nothrow_constructible<T, U>,
                             std::is_nothrow_constructible<E, Gr>>)
      : has_value_(x.has_value_) {
    if (has_value_) {
      std::construct_at(std::addressof(val_), std::move(x).val_);
    } else {
      std::construct_at(std::addressof(unex_), std::move(x).unex_);
    }
  }

  template <typename U = T>
    requires(!std::is_same_v<std::remove_cvref_t<U>, Expected>) &&
                (!std::is_same_v<std::remove_cvref_t<U>, std::in_place_t>) &&
                (!expected::is_unexpected<std::remove_cvref_t<U>>) &&
                std::is_constructible_v<T, U>
  constexpr explicit(!std::is_convertible_v<U, T>)
      Expected(U&& v) noexcept(std::is_nothrow_constructible_v<T, U>)
      : val_(std::forward<U>(v)), has_value_(true) {}

  template <typename Gr = E>
    requires std::is_constructible_v<E, const Gr&>
  constexpr explicit(!std::is_convertible_v<const Gr&, E>)
      Expected(const UnExpected<Gr>& u) noexcept(
          std::is_nothrow_constructible_v<E, const Gr&>)
      : unex_(u.Error()), has_value_(false) {}

  template <typename Gr = E>
    requires std::is_constructible_v<E, Gr>
  constexpr explicit(!std::is_convertible_v<Gr, E>) Expected(
      UnExpected<Gr>&& u) noexcept(std::is_nothrow_constructible_v<E, Gr>)
      : unex_(std::move(u).Error()), has_value_(false) {}

  template <typename... Args>
    requires std::is_constructible_v<T, Args...>
  constexpr explicit Expected(std::in_place_t, Args&&... args) noexcept(
      std::is_nothrow_constructible_v<T, Args...>)
      : val_(std::forward<Args>(args)...), has_value_(true) {}

  template <typename U, typename... Args>
    requires std::is_constructible_v<T, std::initializer_list<U>&, Args...>
  constexpr explicit Expected(
      std::in_place_t, std::initializer_list<U> il,
      Args&&... args) noexcept(std::
                                   is_nothrow_constructible_v<
                                       T, std::initializer_list<U>&, Args...>)
      : val_(il, std::forward<Args>(args)...), has_value_(true) {}

  template <typename... Args>
    requires std::is_constructible_v<E, Args...>
  constexpr explicit Expected(unexpect_t, Args&&... args) noexcept(
      std::is_nothrow_constructible_v<E, Args...>)
      : unex_(std::forward<Args>(args)...), has_value_(false) {}

  template <typename U, typename... Args>
    requires std::is_constructible_v<E, std::initializer_list<U>&, Args...>
  constexpr explicit Expected(
      unexpect_t, std::initializer_list<U> il,
      Args&&... args) noexcept(std::
                                   is_nothrow_constructible_v<
                                       E, std::initializer_list<U>&, Args...>)
      : unex_(il, std::forward<Args>(args)...), has_value_(false) {}

  constexpr ~Expected() = default;

  constexpr ~Expected()
    requires(!std::is_trivially_destructible_v<T>) ||
            (!std::is_trivially_destructible_v<E>)
  {
    if (has_value_) {
      std::destroy_at(std::addressof(val_));
    } else {
      std::destroy_at(std::addressof(unex_));
    }
  }

  // assignment

  Expected& operator=(const Expected&) = delete;

  constexpr Expected& operator=(const Expected& x) noexcept(
      std::conjunction_v<std::is_nothrow_copy_constructible<T>,
                         std::is_nothrow_copy_constructible<E>,
                         std::is_nothrow_copy_assignable<T>,
                         std::is_nothrow_copy_assignable<E>>)
    requires std::is_copy_assignable_v<T> && std::is_copy_constructible_v<T> &&
             std::is_copy_assignable_v<E> && std::is_copy_constructible_v<E> &&
             (std::is_nothrow_move_constructible_v<T> ||
              std::is_nothrow_move_constructible_v<E>)
  {
    if (x.has_value_) {
      this->AssignValue(x.val_);
    } else {
      this->AssignUnExpect(x.unex_);
    }

    return *this;
  }

  constexpr Expected& operator=(Expected&& x) noexcept(
      std::conjunction_v<std::is_nothrow_move_constructible<T>,
                         std::is_nothrow_move_constructible<E>,
                         std::is_nothrow_move_assignable<T>,
                         std::is_nothrow_move_assignable<E>>)
    requires std::is_move_assignable_v<T> && std::is_move_constructible_v<T> &&
             std::is_move_assignable_v<E> && std::is_move_constructible_v<E> &&
             (std::is_nothrow_move_constructible_v<T> ||
              std::is_nothrow_move_constructible_v<E>)
  {
    if (x.has_value_) {
      AssignValue(std::move(x.val_));
    } else {
      AssignUnExpect(std::move(x.unex_));
    }

    return *this;
  }

  template <typename U = T>
    requires(!std::is_same_v<Expected, std::remove_cvref_t<U>>) &&
            (!expected::is_unexpected<std::remove_cvref_t<U>>) &&
            std::is_constructible_v<T, U> && std::is_assignable_v<T&, U> &&
            (std::is_nothrow_constructible_v<T, U> ||
             std::is_nothrow_move_constructible_v<T> ||
             std::is_nothrow_move_constructible_v<E>)
  constexpr Expected& operator=(U&& v) {
    AssignValue(std::forward<U>(v));

    return *this;
  }

  template <typename Gr>
    requires std::is_constructible_v<E, const Gr&> &&
             std::is_assignable_v<E&, const Gr&> &&
             (std::is_nothrow_constructible_v<E, const Gr&> ||
              std::is_nothrow_move_constructible_v<T> ||
              std::is_nothrow_move_constructible_v<E>)
  constexpr Expected& operator=(const UnExpected<Gr>& e) {
    AssignUnExpect(e.Error());

    return *this;
  }

  template <typename Gr>
    requires std::is_constructible_v<E, Gr> && std::is_assignable_v<E&, Gr> &&
             (std::is_nothrow_constructible_v<E, Gr> ||
              std::is_nothrow_move_constructible_v<T> ||
              std::is_nothrow_move_constructible_v<E>)
  constexpr Expected& operator=(UnExpected<Gr>&& e) {
    AssignUnExpect(std::move(e).Error());

    return *this;
  }

  // modifiers

  template <typename... Args>
    requires std::is_nothrow_constructible_v<T, Args...>
  constexpr T& Emplace(Args&&... args) noexcept {
    if (has_value_) {
      std::destroy_at(std::addressof(val_));
    } else {
      std::destroy_at(std::addressof(unex_));
      has_value_ = true;
    }
    std::construct_at(std::addressof(val_), std::forward<Args>(args)...);

    return val_;
  }

  template <typename U, typename... Args>
    requires std::is_nothrow_constructible_v<T, std::initializer_list<U>&,
                                             Args...>
  constexpr T& Emplace(std::initializer_list<U> il, Args&&... args) noexcept {
    if (has_value_) {
      std::destroy_at(std::addressof(val_));
    } else {
      std::destroy_at(std::addressof(unex_));
      has_value_ = true;
    }
    std::construct_at(std::addressof(val_), il, std::forward<Args>(args)...);

    return val_;
  }

  // swap
  constexpr void swap(Expected& x) noexcept(
      std::conjunction_v<std::is_nothrow_move_constructible<T>,
                         std::is_nothrow_move_constructible<E>,
                         std::is_nothrow_swappable<T&>,
                         std::is_nothrow_swappable<E&>>)
    requires std::is_swappable_v<T> && std::is_swappable_v<E> &&
             std::is_move_constructible_v<T> &&
             std::is_move_constructible_v<E> &&
             (std::is_nothrow_move_constructible_v<T> ||
              std::is_nothrow_move_constructible_v<E>)
  {
    if (has_value_) {
      if (x.has_value_) {
        using std::swap;

        swap(val_, x.val_);
      } else
        this->SwapValueAndUnExpext(x);
    } else {
      if (x.has_value_)
        x.SwapValueAndUnExpext(*this);
      else {
        using std::swap;
        swap(unex_, x.unex_);
      }
    }
  }

  // observers

  [[nodiscard]] constexpr const T* operator->() const noexcept {
    assert(has_value_);

    return std::addressof(val_);
  }

  [[nodiscard]] constexpr T* operator->() noexcept {
    assert(has_value_);

    return std::addressof(val_);
  }

  [[nodiscard]] constexpr const T& operator*() const& noexcept {
    assert(has_value_);

    return val_;
  }

  [[nodiscard]] constexpr T& operator*() & noexcept {
    assert(has_value_);

    return val_;
  }

  [[nodiscard]] constexpr const T&& operator*() const&& noexcept {
    assert(has_value_);

    return std::move(val_);
  }

  [[nodiscard]] constexpr T&& operator*() && noexcept {
    assert(has_value_);

    return std::move(val_);
  }

  [[nodiscard]] constexpr explicit operator bool() const noexcept {
    return has_value_;
  }

  [[nodiscard]] constexpr bool HasValue() const noexcept { return has_value_; }

  constexpr const T& Value() const& {
    if (has_value_) [[likely]]
      return val_;

    throw(bad_expected_access<E>(unex_));
  }

  constexpr T& Value() & {
    if (has_value_) [[likely]]
      return val_;

    const auto& unex = unex_;

    throw(bad_expected_access<E>(unex));
  }

  constexpr const T&& Value() const&& {
    if (has_value_) [[likely]]
      return std::move(val_);

    throw(bad_expected_access<E>(std::move(unex_)));
  }

  constexpr T&& Value() && {
    if (has_value_) [[likely]]
      return std::move(val_);

    throw(bad_expected_access<E>(std::move(unex_)));
  }

  constexpr const E& Error() const& noexcept {
    assert(!has_value_);

    return unex_;
  }

  constexpr E& Error() & noexcept {
    assert(!has_value_);

    return unex_;
  }

  constexpr const E&& Error() const&& noexcept {
    assert(!has_value_);

    return std::move(unex_);
  }

  constexpr E&& Error() && noexcept {
    assert(!has_value_);

    return std::move(unex_);
  }

  template <typename U>
  constexpr T ValueOr(U&& v) const& noexcept(
      std::conjunction_v<std::is_nothrow_copy_constructible<T>,
                         std::is_nothrow_convertible<U, T>>) {
    static_assert(std::is_copy_constructible_v<T>);
    static_assert(std::is_convertible_v<U, T>);

    if (has_value_) return val_;

    return static_cast<T>(std::forward<U>(v));
  }

  template <typename U>
  constexpr T ValueOr(U&& v) && noexcept(
      std::conjunction_v<std::is_nothrow_move_constructible<T>,
                         std::is_nothrow_convertible<U, T>>) {
    static_assert(std::is_move_constructible_v<T>);
    static_assert(std::is_convertible_v<U, T>);

    if (has_value_) return std::move(val_);

    return static_cast<T>(std::forward<U>(v));
  }

  template <typename Gr = E>
  constexpr E ErrorOr(Gr&& e) const& {
    static_assert(std::is_copy_constructible_v<E>);
    static_assert(std::is_convertible_v<Gr, E>);

    if (has_value_) return std::forward<Gr>(e);

    return unex_;
  }

  template <typename Gr = E>
  constexpr E ErrorOr(Gr&& e) && {
    static_assert(std::is_move_constructible_v<E>);
    static_assert(std::is_convertible_v<Gr, E>);

    if (has_value_) return std::forward<Gr>(e);

    return std::move(unex_);
  }

  // monadic operations

  template <typename Fn>
    requires std::is_constructible_v<E, E&>
  constexpr auto AndThen(Fn&& f) & {
    using U = expected::result<Fn, T&>;
    static_assert(expected::is_expected<U>,
                  "the function passed to std::expected<T, E>::AndThen "
                  "must return a std::expected");
    static_assert(std::is_same_v<typename U::error_type, E>,
                  "the function passed to std::expected<T, E>::AndThen "
                  "must return a std::expected with the same error_type");

    if (HasValue()) {
      return std::invoke(std::forward<Fn>(f), val_);
    } else {
      return U(unexpect, unex_);
    }
  }

  template <typename Fn>
    requires std::is_constructible_v<E, const E&>
  constexpr auto AndThen(Fn&& f) const& {
    using U = expected::result<Fn, const T&>;
    static_assert(expected::is_expected<U>,
                  "the function passed to std::expected<T, E>::AndThen "
                  "must return a std::expected");
    static_assert(std::is_same_v<typename U::error_type, E>,
                  "the function passed to std::expected<T, E>::AndThen "
                  "must return a std::expected with the same error_type");

    if (HasValue()) {
      return std::invoke(std::forward<Fn>(f), val_);
    } else {
      return U(unexpect, unex_);
    }
  }

  template <typename Fn>
    requires std::is_constructible_v<E, E>
  constexpr auto AndThen(Fn&& f) && {
    using U = expected::result<Fn, T&&>;
    static_assert(expected::is_expected<U>,
                  "the function passed to std::expected<T, E>::AndThen "
                  "must return a std::expected");
    static_assert(std::is_same_v<typename U::error_type, E>,
                  "the function passed to std::expected<T, E>::AndThen "
                  "must return a std::expected with the same error_type");

    if (HasValue())
      return std::invoke(std::forward<Fn>(f), std::move(val_));
    else
      return U(unexpect, std::move(unex_));
  }

  template <typename Fn>
    requires std::is_constructible_v<E, const E>
  constexpr auto AndThen(Fn&& f) const&& {
    using U = expected::result<Fn, const T&&>;
    static_assert(expected::is_expected<U>,
                  "the function passed to std::expected<T, E>::AndThen "
                  "must return a std::expected");
    static_assert(std::is_same_v<typename U::error_type, E>,
                  "the function passed to std::expected<T, E>::AndThen "
                  "must return a std::expected with the same error_type");

    if (HasValue()) {
      return std::invoke(std::forward<Fn>(f), std::move(val_));
    } else {
      return U(unexpect, std::move(unex_));
    }
  }

  template <typename Fn>
    requires std::is_constructible_v<T, T&>
  constexpr auto OrElse(Fn&& f) & {
    using Gr = expected::result<Fn, E&>;
    static_assert(expected::is_expected<Gr>,
                  "the function passed to std::expected<T, E>::OrElse "
                  "must return a std::expected");
    static_assert(std::is_same_v<typename Gr::value_type, T>,
                  "the function passed to std::expected<T, E>::OrElse "
                  "must return a std::expected with the same value_type");

    if (HasValue()) {
      return Gr(std::in_place, val_);
    } else {
      return std::invoke(std::forward<Fn>(f), unex_);
    }
  }

  template <typename Fn>
    requires std::is_constructible_v<T, const T&>
  constexpr auto OrElse(Fn&& f) const& {
    using Gr = expected::result<Fn, const E&>;
    static_assert(expected::is_expected<Gr>,
                  "the function passed to std::expected<T, E>::OrElse "
                  "must return a std::expected");
    static_assert(std::is_same_v<typename Gr::value_type, T>,
                  "the function passed to std::expected<T, E>::OrElse "
                  "must return a std::expected with the same value_type");

    if (HasValue()) {
      return Gr(std::in_place, val_);
    } else {
      return std::invoke(std::forward<Fn>(f), unex_);
    }
  }

  template <typename Fn>
    requires std::is_constructible_v<T, T>
  constexpr auto OrElse(Fn&& f) && {
    using Gr = expected::result<Fn, E&&>;
    static_assert(expected::is_expected<Gr>,
                  "the function passed to std::expected<T, E>::OrElse "
                  "must return a std::expected");
    static_assert(std::is_same_v<typename Gr::value_type, T>,
                  "the function passed to std::expected<T, E>::OrElse "
                  "must return a std::expected with the same value_type");

    if (HasValue()) {
      return Gr(std::in_place, std::move(val_));
    } else {
      return std::invoke(std::forward<Fn>(f), std::move(unex_));
    }
  }

  template <typename Fn>
    requires std::is_constructible_v<T, const T>
  constexpr auto OrElse(Fn&& f) const&& {
    using Gr = expected::result<Fn, const E&&>;
    static_assert(expected::is_expected<Gr>,
                  "the function passed to std::expected<T, E>::OrElse "
                  "must return a std::expected");
    static_assert(std::is_same_v<typename Gr::value_type, T>,
                  "the function passed to std::expected<T, E>::OrElse "
                  "must return a std::expected with the same value_type");

    if (HasValue()) {
      return Gr(std::in_place, std::move(val_));
    } else {
      return std::invoke(std::forward<Fn>(f), std::move(unex_));
    }
  }

  template <typename Fn>
    requires std::is_constructible_v<E, E&>
  constexpr auto Transform(Fn&& f) & {
    using U = expected::result_xform<Fn, T&>;
    using Res = Expected<U, E>;

    if (HasValue()) {
      return Res(in_place_inv{},
                 [&]() { return std::invoke(std::forward<Fn>(f), val_); });
    } else {
      return Res(unexpect, unex_);
    }
  }

  template <typename Fn>
    requires std::is_constructible_v<E, const E&>
  constexpr auto Transform(Fn&& f) const& {
    using U = expected::result_xform<Fn, const T&>;
    using Res = Expected<U, E>;

    if (HasValue()) {
      return Res(in_place_inv{},
                 [&]() { return std::invoke(std::forward<Fn>(f), val_); });
    } else {
      return Res(unexpect, unex_);
    }
  }

  template <typename Fn>
    requires std::is_constructible_v<E, E>
  constexpr auto Transform(Fn&& f) && {
    using U = expected::result_xform<Fn, T>;
    using Res = Expected<U, E>;

    if (HasValue()) {
      return Res(in_place_inv{}, [&]() {
        return std::invoke(std::forward<Fn>(f), std::move(val_));
      });
    } else {
      return Res(unexpect, std::move(unex_));
    }
  }

  template <typename Fn>
    requires std::is_constructible_v<E, const E>
  constexpr auto Transform(Fn&& f) const&& {
    using U = expected::result_xform<Fn, const T>;
    using Res = Expected<U, E>;

    if (HasValue()) {
      return Res(in_place_inv{}, [&]() {
        return std::invoke(std::forward<Fn>(f), std::move(val_));
      });
    } else {
      return Res(unexpect, std::move(unex_));
    }
  }

  template <typename Fn>
    requires std::is_constructible_v<T, T&>
  constexpr auto TransformError(Fn&& f) & {
    using Gr = expected::result_xform<Fn, E&>;
    using Res = Expected<T, Gr>;

    if (HasValue()) {
      return Res(std::in_place, val_);
    } else {
      return Res(unexpect_inv{},
                 [&]() { return std::invoke(std::forward<Fn>(f), unex_); });
    }
  }

  template <typename Fn>
    requires std::is_constructible_v<T, const T&>
  constexpr auto TransformError(Fn&& f) const& {
    using Gr = expected::result_xform<Fn, const E&>;
    using Res = Expected<T, Gr>;

    if (HasValue()) {
      return Res(std::in_place, val_);
    } else {
      return Res(unexpect_inv{},
                 [&]() { return std::invoke(std::forward<Fn>(f), unex_); });
    }
  }

  template <typename Fn>
    requires std::is_constructible_v<T, T>
  constexpr auto TransformError(Fn&& f) && {
    using Gr = expected::result_xform<Fn, E&&>;
    using Res = Expected<T, Gr>;

    if (HasValue()) {
      return Res(std::in_place, std::move(val_));
    } else {
      return Res(unexpect_inv{}, [&]() {
        return std::invoke(std::forward<Fn>(f), std::move(unex_));
      });
    }
  }

  template <typename Fn>
    requires std::is_constructible_v<T, const T>
  constexpr auto TransformError(Fn&& f) const&& {
    using Gr = expected::result_xform<Fn, const E&&>;
    using Res = Expected<T, Gr>;

    if (HasValue()) {
      return Res(std::in_place, std::move(val_));
    } else {
      return Res(unexpect_inv{}, [&]() {
        return std::invoke(std::forward<Fn>(f), std::move(unex_));
      });
    }
  }

  // equality operators

  template <typename U, typename E2>
    requires(!std::is_void_v<U>)
  friend constexpr bool operator==(const Expected& x, const Expected<U, E2>& y)
  // FIXME: noexcept(noexcept(bool(*x == *y))
  // && noexcept(bool(x.error() == y.error())))
  {
    if (x.HasValue()) {
      return y.HasValue() && bool(*x == *y);
    } else {
      return !y.HasValue() && bool(x.Error() == y.Error());
    }
  }

  template <typename U>
  friend constexpr bool operator==(const Expected& x, const U& v)
  // FIXME: noexcept(noexcept(bool(*x == v)))
  {
    return x.HasValue() && bool(*x == v);
  }

  template <typename E2>
  friend constexpr bool operator==(const Expected& x, const UnExpected<E2>& e)
  // FIXME: noexcept(noexcept(bool(x.error() == e.error())))
  {
    return !x.HasValue() && bool(x.Error() == e.Error());
  }

  friend constexpr void swap(Expected& x,
                             Expected& y) noexcept(noexcept(x.swap(y)))
    requires requires { x.swap(y); }
  {
    x.swap(y);
  }

 private:
  template <typename, typename>
  friend class Expected;

  template <typename V>
  constexpr void AssignValue(V&& v) {
    if (has_value_) {
      val_ = std::forward<V>(v);
    } else {
      expected::reinit(std::addressof(val_), std::addressof(unex_),
                       std::forward<V>(v));
      has_value_ = true;
    }
  }

  template <typename V>
  constexpr void AssignUnExpect(V&& v) {
    if (has_value_) {
      expected::reinit(std::addressof(unex_), std::addressof(val_),
                       std::forward<V>(v));
      has_value_ = false;
    } else
      unex_ = std::forward<V>(v);
  }

  // Swap two expected objects when only one has a value.
  // Precondition: this->has_value_ && !rhs.has_value_
  constexpr void SwapValueAndUnExpext(Expected& rhs) noexcept(
      std::conjunction_v<std::is_nothrow_move_constructible<E>,
                         std::is_nothrow_move_constructible<T>>) {
    if constexpr (std::is_nothrow_move_constructible_v<E>) {
      expected::Guard<E> guard(rhs.unex_);
      std::construct_at(std::addressof(rhs.val_),
                        std::move(val_));  // might throw
      rhs.has_value_ = true;
      std::destroy_at(std::addressof(val_));
      std::construct_at(std::addressof(unex_), guard.Release());
      has_value_ = false;
    } else {
      expected::Guard<T> guard(val_);
      std::construct_at(std::addressof(unex_),
                        std::move(rhs.unex_));  // might throw
      has_value_ = false;
      std::destroy_at(std::addressof(rhs.unex_));
      std::construct_at(std::addressof(rhs.val_), guard.Release());
      rhs.has_value_ = true;
    }
  }

  using in_place_inv = expected::in_place_inv;
  using unexpect_inv = expected::unexpect_inv;

  template <typename Fn>
  explicit constexpr Expected(in_place_inv, Fn&& fn)
      : val_(std::forward<Fn>(fn)()), has_value_(true) {}

  template <typename Fn>
  explicit constexpr Expected(unexpect_inv, Fn&& fn)
      : unex_(std::forward<Fn>(fn)()), has_value_(false) {}

  union {
    T val_;
    E unex_;
  };

  bool has_value_;
};

// Partial specialization for std::expected<cv void, E>
template <typename T, typename E>
  requires std::is_void_v<T>
class Expected<T, E> {
  static_assert(expected::can_be_unexpected<E>);

  template <typename U, typename Err, typename Unex = UnExpected<E>>
  static constexpr bool cons_from_expected =
      std::disjunction_v<std::is_constructible<Unex, Expected<U, Err>&>,
                         std::is_constructible<Unex, Expected<U, Err>>,
                         std::is_constructible<Unex, const Expected<U, Err>&>,
                         std::is_constructible<Unex, const Expected<U, Err>>>;

  template <typename U>
  static constexpr bool same_val = std::is_same_v<typename U::value_type, T>;

  template <typename U>
  static constexpr bool same_err = std::is_same_v<typename U::error_type, E>;

 public:
  using value_type = T;
  using error_type = E;
  using unexpected_type = UnExpected<E>;

  template <typename U>
  using rebind = Expected<U, error_type>;

  constexpr Expected() noexcept : void_(), has_value_(true) {}

  Expected(const Expected&) = default;

  constexpr Expected(const Expected& x) noexcept(
      std::is_nothrow_copy_constructible_v<E>)
    requires std::is_copy_constructible_v<E> &&
                 (!std::is_trivially_copy_constructible_v<E>)
      : void_(), has_value_(x.has_value_) {
    if (!has_value_) std::construct_at(std::addressof(unex_), x.unex_);
  }

  Expected(Expected&&) = default;

  constexpr Expected(Expected&& x) noexcept(
      std::is_nothrow_move_constructible_v<E>)
    requires std::is_move_constructible_v<E> &&
                 (!std::is_trivially_move_constructible_v<E>)
      : void_(), has_value_(x.has_value_) {
    if (!has_value_)
      std::construct_at(std::addressof(unex_), std::move(x).unex_);
  }

  template <typename U, typename Gr>
    requires std::is_void_v<U> && std::is_constructible_v<E, const Gr&> &&
                 (!cons_from_expected<U, Gr>)
  constexpr explicit(!std::is_convertible_v<const Gr&, E>)
      Expected(const Expected<U, Gr>& x) noexcept(
          std::is_nothrow_constructible_v<E, const Gr&>)
      : void_(), has_value_(x.has_value_) {
    if (!has_value_) std::construct_at(std::addressof(unex_), x.unex_);
  }

  template <typename U, typename Gr>
    requires std::is_void_v<U> && std::is_constructible_v<E, Gr> &&
                 (!cons_from_expected<U, Gr>)
  constexpr explicit(!std::is_convertible_v<Gr, E>) Expected(
      Expected<U, Gr>&& x) noexcept(std::is_nothrow_constructible_v<E, Gr>)
      : void_(), has_value_(x.has_value_) {
    if (!has_value_)
      std::construct_at(std::addressof(unex_), std::move(x).unex_);
  }

  template <typename Gr = E>
    requires std::is_constructible_v<E, const Gr&>
  constexpr explicit(!std::is_convertible_v<const Gr&, E>)
      Expected(const UnExpected<Gr>& u) noexcept(
          std::is_nothrow_constructible_v<E, const Gr&>)
      : unex_(u.Error()), has_value_(false) {}

  template <typename Gr = E>
    requires std::is_constructible_v<E, Gr>
  constexpr explicit(!std::is_convertible_v<Gr, E>) Expected(
      UnExpected<Gr>&& u) noexcept(std::is_nothrow_constructible_v<E, Gr>)
      : unex_(std::move(u).Error()), has_value_(false) {}

  constexpr explicit Expected(std::in_place_t) noexcept : Expected() {}

  template <typename... Args>
    requires std::is_constructible_v<E, Args...>
  constexpr explicit Expected(unexpect_t, Args&&... args) noexcept(
      std::is_nothrow_constructible_v<E, Args...>)
      : unex_(std::forward<Args>(args)...), has_value_(false) {}

  template <typename U, typename... Args>
    requires std::is_constructible_v<E, std::initializer_list<U>&, Args...>
  constexpr explicit Expected(
      unexpect_t, std::initializer_list<U> il,
      Args&&... args) noexcept(std::
                                   is_nothrow_constructible_v<
                                       E, std::initializer_list<U>&, Args...>)
      : unex_(il, std::forward<Args>(args)...), has_value_(false) {}

  constexpr ~Expected() = default;

  constexpr ~Expected()
    requires(!std::is_trivially_destructible_v<E>)
  {
    if (!has_value_) std::destroy_at(std::addressof(unex_));
  }

  // assignment

  Expected& operator=(const Expected&) = delete;

  constexpr Expected& operator=(const Expected& x) noexcept(
      std::conjunction_v<std::is_nothrow_copy_constructible<E>,
                         std::is_nothrow_copy_assignable<E>>)
    requires std::is_copy_constructible_v<E> && std::is_copy_assignable_v<E>
  {
    if (x.has_value_) {
      Emplace();
    } else {
      AssignUnExpect(x.unex_);
    }

    return *this;
  }

  constexpr Expected& operator=(Expected&& x) noexcept(
      std::conjunction_v<std::is_nothrow_move_constructible<E>,
                         std::is_nothrow_move_assignable<E>>)
    requires std::is_move_constructible_v<E> && std::is_move_assignable_v<E>
  {
    if (x.has_value_) {
      Emplace();
    } else {
      AssignUnExpect(std::move(x.unex_));
    }

    return *this;
  }

  template <typename Gr>
    requires std::is_constructible_v<E, const Gr&> &&
             std::is_assignable_v<E&, const Gr&>
  constexpr Expected& operator=(const UnExpected<Gr>& e) {
    AssignUnExpect(e.Error());

    return *this;
  }

  template <typename Gr>
    requires std::is_constructible_v<E, Gr> && std::is_assignable_v<E&, Gr>
  constexpr Expected& operator=(UnExpected<Gr>&& e) {
    AssignUnExpect(std::move(e.Error()));

    return *this;
  }

  // modifiers

  constexpr void Emplace() noexcept {
    if (!has_value_) {
      std::destroy_at(std::addressof(unex_));
      has_value_ = true;
    }
  }

  // swap
  constexpr void swap(Expected& x) noexcept(
      std::conjunction_v<std::is_nothrow_swappable<E&>,
                         std::is_nothrow_move_constructible<E>>)
    requires std::is_swappable_v<E> && std::is_move_constructible_v<E>
  {
    if (has_value_) {
      if (!x.has_value_) {
        std::construct_at(std::addressof(unex_),
                          std::move(x.unex_));  // might throw
        std::destroy_at(std::addressof(x.unex_));
        has_value_ = false;
        x.has_value_ = true;
      }
    } else {
      if (x.has_value_) {
        std::construct_at(std::addressof(x.unex_),
                          std::move(unex_));  // might throw
        std::destroy_at(std::addressof(unex_));
        has_value_ = true;
        x.has_value_ = false;
      } else {
        using std::swap;
        swap(unex_, x.unex_);
      }
    }
  }

  // observers

  [[nodiscard]] constexpr explicit operator bool() const noexcept {
    return has_value_;
  }

  [[nodiscard]] constexpr bool HasValue() const noexcept { return has_value_; }

  constexpr void operator*() const noexcept { __glibcxx_assert(has_value_); }

  constexpr void Value() const& {
    if (has_value_) [[likely]]
      return;

    throw(bad_expected_access<E>(unex_));
  }

  constexpr void Value() && {
    if (has_value_) [[likely]]
      return;

    throw(bad_expected_access<E>(std::move(unex_)));
  }

  constexpr const E& Error() const& noexcept {
    assert(!has_value_);

    return unex_;
  }

  constexpr E& Error() & noexcept {
    assert(!has_value_);

    return unex_;
  }

  constexpr const E&& Error() const&& noexcept {
    assert(!has_value_);

    return std::move(unex_);
  }

  constexpr E&& Error() && noexcept {
    assert(!has_value_);

    return std::move(unex_);
  }

  template <typename Gr = E>
  constexpr E ErrorOr(Gr&& e) const& {
    static_assert(std::is_copy_constructible_v<E>);
    static_assert(std::is_convertible_v<Gr, E>);

    if (has_value_) return std::forward<Gr>(e);

    return unex_;
  }

  template <typename Gr = E>
  constexpr E ErrorOr(Gr&& e) && {
    static_assert(std::is_move_constructible_v<E>);
    static_assert(std::is_convertible_v<Gr, E>);

    if (has_value_) return std::forward<Gr>(e);

    return std::move(unex_);
  }

  // monadic operations

  template <typename Fn>
    requires std::is_constructible_v<E, E&>
  constexpr auto AndThen(Fn&& f) & {
    using U = expected::result0<Fn>;
    static_assert(expected::is_expected<U>);
    static_assert(std::is_same_v<typename U::error_type, E>);

    if (HasValue()) {
      return std::invoke(std::forward<Fn>(f));
    } else {
      return U(unexpect, unex_);
    }
  }

  template <typename Fn>
    requires std::is_constructible_v<E, const E&>
  constexpr auto AndThen(Fn&& f) const& {
    using U = expected::result0<Fn>;
    static_assert(expected::is_expected<U>);
    static_assert(std::is_same_v<typename U::error_type, E>);

    if (HasValue()) {
      return std::invoke(std::forward<Fn>(f));
    } else {
      return U(unexpect, unex_);
    }
  }

  template <typename Fn>
    requires std::is_constructible_v<E, E>
  constexpr auto AndThen(Fn&& f) && {
    using U = expected::result0<Fn>;
    static_assert(expected::is_expected<U>);
    static_assert(std::is_same_v<typename U::error_type, E>);

    if (HasValue()) {
      return std::invoke(std::forward<Fn>(f));
    } else {
      return U(unexpect, std::move(unex_));
    }
  }

  template <typename Fn>
    requires std::is_constructible_v<E, const E>
  constexpr auto AndThen(Fn&& f) const&& {
    using U = expected::result0<Fn>;
    static_assert(expected::is_expected<U>);
    static_assert(std::is_same_v<typename U::error_type, E>);

    if (HasValue()) {
      return std::invoke(std::forward<Fn>(f));
    } else {
      return U(unexpect, std::move(unex_));
    }
  }

  template <typename Fn>
  constexpr auto OrElse(Fn&& f) & {
    using Gr = expected::result<Fn, E&>;
    static_assert(expected::is_expected<Gr>);
    static_assert(std::is_same_v<typename Gr::value_type, T>);

    if (HasValue()) {
      return Gr();
    } else {
      return std::invoke(std::forward<Fn>(f), unex_);
    }
  }

  template <typename Fn>
  constexpr auto OrElse(Fn&& f) const& {
    using Gr = expected::result<Fn, const E&>;
    static_assert(expected::is_expected<Gr>);
    static_assert(std::is_same_v<typename Gr::value_type, T>);

    if (HasValue()) {
      return Gr();
    } else {
      return std::invoke(std::forward<Fn>(f), unex_);
    }
  }

  template <typename Fn>
  constexpr auto OrElse(Fn&& f) && {
    using Gr = expected::result<Fn, E&&>;
    static_assert(expected::is_expected<Gr>);
    static_assert(std::is_same_v<typename Gr::value_type, T>);

    if (HasValue()) {
      return Gr();
    } else {
      return std::invoke(std::forward<Fn>(f), std::move(unex_));
    }
  }

  template <typename Fn>
  constexpr auto OrElse(Fn&& f) const&& {
    using Gr = expected::result<Fn, const E&&>;
    static_assert(expected::is_expected<Gr>);
    static_assert(std::is_same_v<typename Gr::value_type, T>);

    if (HasValue()) {
      return Gr();
    } else {
      return std::invoke(std::forward<Fn>(f), std::move(unex_));
    }
  }

  template <typename Fn>
    requires std::is_constructible_v<E, E&>
  constexpr auto Transform(Fn&& f) & {
    using U = expected::result0_xform<Fn>;
    using Res = Expected<U, E>;

    if (HasValue()) {
      return Res(in_place_inv{}, std::forward<Fn>(f));
    } else {
      return Res(unexpect, unex_);
    }
  }

  template <typename Fn>
    requires std::is_constructible_v<E, const E&>
  constexpr auto Transform(Fn&& f) const& {
    using U = expected::result0_xform<Fn>;
    using Res = Expected<U, E>;

    if (HasValue()) {
      return Res(in_place_inv{}, std::forward<Fn>(f));
    } else {
      return Res(unexpect, unex_);
    }
  }

  template <typename Fn>
    requires std::is_constructible_v<E, E>
  constexpr auto Transform(Fn&& f) && {
    using U = expected::result0_xform<Fn>;
    using Res = Expected<U, E>;

    if (HasValue())
      return Res(in_place_inv{}, std::forward<Fn>(f));
    else
      return Res(unexpect, std::move(unex_));
  }

  template <typename Fn>
    requires std::is_constructible_v<E, const E>
  constexpr auto Transform(Fn&& f) const&& {
    using U = expected::result0_xform<Fn>;
    using Res = Expected<U, E>;

    if (HasValue()) {
      return Res(in_place_inv{}, std::forward<Fn>(f));
    } else {
      return Res(unexpect, std::move(unex_));
    }
  }

  template <typename Fn>
  constexpr auto TransformError(Fn&& f) & {
    using Gr = expected::result_xform<Fn, E&>;
    using Res = Expected<T, Gr>;

    if (HasValue()) {
      return Res();
    } else {
      return Res(unexpect_inv{},
                 [&]() { return std::invoke(std::forward<Fn>(f), unex_); });
    }
  }

  template <typename Fn>
  constexpr auto TransformError(Fn&& f) const& {
    using Gr = expected::result_xform<Fn, const E&>;
    using Res = Expected<T, Gr>;

    if (HasValue()) {
      return Res();
    } else {
      return Res(unexpect_inv{},
                 [&]() { return std::invoke(std::forward<Fn>(f), unex_); });
    }
  }

  template <typename Fn>
  constexpr auto TransformError(Fn&& f) && {
    using Gr = expected::result_xform<Fn, E&&>;
    using Res = Expected<T, Gr>;

    if (HasValue()) {
      return Res();
    } else {
      return Res(unexpect_inv{}, [&]() {
        return std::invoke(std::forward<Fn>(f), std::move(unex_));
      });
    }
  }

  template <typename Fn>
  constexpr auto TransformError(Fn&& f) const&& {
    using Gr = expected::result_xform<Fn, const E&&>;
    using Res = Expected<T, Gr>;

    if (HasValue()) {
      return Res();
    } else {
      return Res(unexpect_inv{}, [&]() {
        return std::invoke(std::forward<Fn>(f), std::move(unex_));
      });
    }
  }

  // equality operators

  template <typename U, typename E2>
    requires std::is_void_v<U>
  friend constexpr bool operator==(const Expected& x, const Expected<U, E2>& y)
  // FIXME: noexcept(noexcept(bool(x.error() == y.error())))
  {
    if (x.HasValue()) {
      return y.HasValue();
    } else {
    }
    return !y.HasValue() && bool(x.Error() == y.Error());
  }

  template <typename E2>
  friend constexpr bool operator==(const Expected& x, const UnExpected<E2>& e)
  // FIXME: noexcept(noexcept(bool(x.error() == e.error())))
  {
    return !x.HasValue() && bool(x.Error() == e.Error());
  }

  friend constexpr void swap(Expected& x,
                             Expected& y) noexcept(noexcept(x.swap(y)))
    requires requires { x.swap(y); }
  {
    x.swap(y);
  }

 private:
  template <typename, typename>
  friend class Expected;

  template <typename V>
  constexpr void AssignUnExpect(V&& v) {
    if (has_value_) {
      std::construct_at(std::addressof(unex_), std::forward<V>(v));
      has_value_ = false;
    } else
      unex_ = std::forward<V>(v);
  }

  using in_place_inv = expected::in_place_inv;
  using unexpect_inv = expected::unexpect_inv;

  template <typename Fn>
  explicit constexpr Expected(in_place_inv, Fn&& fn)
      : void_(), has_value_(true) {
    std::forward<Fn>(fn)();
  }

  template <typename Fn>
  explicit constexpr Expected(unexpect_inv, Fn&& fn)
      : unex_(std::forward<Fn>(fn)()), has_value_(false) {}

  union {
    struct {
    } void_;
    E unex_;
  };

  bool has_value_;
};
/// @}

}  // namespace chibicpp