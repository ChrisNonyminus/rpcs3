#pragma once

#include "util/types.hpp"
#include "Utilities/mutex.h"

#include <memory>
#include <vector>
#include <map>
#include <typeinfo>

#include "util/serialization.hpp"
#include "util/fixed_typemap.hpp"

extern stx::manual_typemap<void, 0x20'00000, 128> g_fixed_typemap;

constexpr auto* g_fxo = &g_fixed_typemap;

extern thread_local std::string_view g_tls_serialize_name;

template <typename T, typename U = T>
inline U* fxo_serialize_body(utils::serial* ar)
{
	if (ar)
	{
		g_tls_serialize_name = g_fxo->get_name<T, U>();
		return g_fxo->init<T, U>(stx::exact_t<utils::serial&>(*ar));
	}

	if constexpr (std::is_constructible_v<U>)
	{
		return g_fxo->init<T, U>();
	}
	else
	{
		// Must be constructed already
		return ensure(static_cast<U*>(g_fxo->try_get<T>()));
	}
}

enum class thread_state : u32;

// Helper namespace
namespace id_manager
{
	// Common global mutex
	extern shared_mutex g_mutex;

	// Last allocated ID for constructors
	extern thread_local u32 g_id;

	// ID traits
	template <typename T, typename = void>
	struct id_traits
	{
		//static_assert(sizeof(T) == 0, "ID object must specify: id_base, id_step, id_count");

		static constexpr u32 base    = 1;     // First ID (N = 0)
		static constexpr u32 step    = 1;     // Any ID: N * id_step + id_base
		static constexpr u32 count   = 65535; // Limit: N < id_count
		static constexpr u32 invalid = 0;
		static constexpr std::pair<u32, u32> invl_range{0, 0};
	};

	template <typename T, typename = void>
	struct invl_range_extract_impl
	{
		static constexpr std::pair<u32, u32> invl_range{0, 0};
	};

	template <typename T>
	struct invl_range_extract_impl<T, std::void_t<decltype(&T::id_invl_range)>>
	{
		static constexpr std::pair<u32, u32> invl_range = T::id_invl_range;
	};

	template <typename T>
	struct id_traits<T, std::void_t<decltype(&T::id_base), decltype(&T::id_step), decltype(&T::id_count)>>
	{
		static constexpr u32 base    = T::id_base;
		static constexpr u32 step    = T::id_step;
		static constexpr u32 count   = T::id_count;
		static constexpr u32 invalid = -+!base;

		static constexpr std::pair<u32, u32> invl_range = invl_range_extract_impl<T>::invl_range;

		static_assert(count && step && u64{step} * (count - 1) + base < u32{umax} + u64{base != 0 ? 1 : 0}, "ID traits: invalid object range");

		// TODO: Add more conditions
		static_assert(!invl_range.second || (u64{invl_range.second} + invl_range.first <= 32 /*....*/ ));
	};

	// Correct usage testing
	template <typename T, typename T2, typename = void>
	struct id_verify : std::integral_constant<bool, std::is_base_of<T, T2>::value>
	{
		// If common case, T2 shall be derived from or equal to T
	};

	template <typename T, typename T2>
	struct id_verify<T, T2, std::void_t<typename T2::id_type>> : std::integral_constant<bool, std::is_same<T, typename T2::id_type>::value>
	{
		// If T2 contains id_type type, T must be equal to it
	};

	static constexpr u32 get_index(u32 id, u32 base, u32 step, u32 count, std::pair<u32, u32> invl_range)
	{
		u32 mask_out = ((1u << invl_range.second) - 1) << invl_range.first;

		// Note: if id is lower than base, diff / step will be higher than count
		u32 diff = (id & ~mask_out) - base;

		if (diff % step)
		{
			// id is invalid, return invalid index
			return count;
		}

		// Get actual index
		return diff / step;
	}

	inline u64 get_hash_from_index(u64 index)
	{
		return index;
	}

	// ID traits
	template <typename T, typename = void>
	struct id_traits_load_func
	{
		static constexpr std::shared_ptr<void>(*load)(utils::serial&) = [](utils::serial& ar) -> std::shared_ptr<void> { return std::make_shared<T>(stx::exact_t<utils::serial&>(ar)); };
	};

	template <typename T>
	struct id_traits_load_func<T, std::void_t<decltype(&T::load)>>
	{
		static constexpr std::shared_ptr<void>(*load)(utils::serial&) = &T::load;
	};

	template <typename T, typename = void>
	struct id_traits_savable_func
	{
		static constexpr bool(*savable)(void*) = [](void*) -> bool { return true; };
	};

	template <typename T>
	struct id_traits_savable_func<T, std::void_t<decltype(&T::savable)>>
	{
		static constexpr bool(*savable)(void* ptr) = [](void* ptr) -> bool { return static_cast<const T*>(ptr)->savable(); };
	};

	struct dummy_construct
	{
		dummy_construct() {}
		dummy_construct(utils::serial&){}
		void save(utils::serial&) {}
	};

	struct typeinfo
	{
	public:
		std::shared_ptr<void>(*load)(utils::serial&);
		void(*save)(utils::serial&, void*);
		bool(*savable)(void* ptr);

		u32 base;
		u32 step;
		u32 count;
		std::pair<u32, u32> invl_range;

		// Get type index
		template <typename T>
		static inline u64 get_index()
		{
			return reinterpret_cast<u64>(&stx::typedata<id_manager::typeinfo, T>());
		}

		template <typename T>
		static typeinfo make_typeinfo()
		{
			typeinfo info{};

			using C = std::conditional_t<std::is_constructible_v<T, stx::exact_t<utils::serial&>>, T, dummy_construct>;

			if constexpr (std::is_same_v<C, T>)
			{
				info =
				{
					+id_traits_load_func<C>::load,
					+[](utils::serial& ar, void* obj) { static_cast<C*>(obj)->save(ar); },
					+id_traits_savable_func<C>::savable,
					id_traits<T>::base, id_traits<T>::step, id_traits<T>::count, id_traits<T>::invl_range,
				};
			}
			else
			{
				info =
				{
					nullptr,
					nullptr,
					nullptr,
					id_traits<T>::base, id_traits<T>::step, id_traits<T>::count, id_traits<T>::invl_range,
				};
			}

			return info;
		}
	};

	// ID value with additional type stored
	class id_key
	{
		u32 m_value;           // ID value
		u64 m_type;            // True object type

	public:
		id_key() = default;

		id_key(u32 value, u64 type)
			: m_value(value)
			, m_type(type)
		{
		}

		u32 value() const
		{
			return m_value;
		}

		u64 type() const
		{
			return m_type;
		}

		operator u32() const
		{
			return m_value;
		}
	};

	template <typename T>
	struct id_map
	{
		std::vector<std::pair<id_key, std::shared_ptr<void>>> vec{}, private_copy{};
		shared_mutex mutex{}; // TODO: Use this instead of global mutex

		id_map()
		{
			// Preallocate memory
			vec.reserve(T::id_count);
		}

		id_map(utils::serial& ar)
		{
			vec.resize(T::id_count);

			while (true)
			{
				// ID, type hash
				std::pair<u32, u64> p{};

				ar(p.first);

				if (p.first == id_traits<T>::invalid)
				{
					// Terminator encountered
					break;
				}

				ar(p.second);

				// Construct each object from information collected

				// Access type information
				const auto& info = *reinterpret_cast<const typeinfo*>(p.second);

				g_id = p.first; // Simulate construction semantics

				auto& obj = vec[get_index(p.first, info.base, info.step, info.count, info.invl_range)]; 
				if (obj.second) continue; // Initialized through dependencies by previous object constructor

				obj.first = id_key(p.first, p.second);
				obj.second = info.load(ar);
			}
		}

		void save(utils::serial& ar)
		{
			for (const auto& p : vec)
			{
				if (!p.second) continue;

				// Save each object with needed information
				auto& info = *reinterpret_cast<const typeinfo*>(get_hash_from_index(p.first.type()));
				if (info.save && info.savable(p.second.get()))
				{
					ar(p.first.value(), get_hash_from_index(p.first.type()));
					info.save(ar, p.second.get());
				}
			}

			// Terminator
			ar(id_traits<T>::invalid);
		}

		template <bool dummy = false> requires (std::is_assignable_v<T&, thread_state>)
		id_map& operator=(thread_state state)
		{
			if (private_copy.empty())
			{
				reader_lock lock(g_mutex);

				// Save all entries
				private_copy = vec;
			}

			// Signal or join threads
			for (const auto& [key, ptr] : private_copy)
			{
				if (ptr)
				{
					*static_cast<T*>(ptr.get()) = state;
				}
			}

			return *this;
		}
	};
}

// Object manager for emulated process. Multiple objects of specified arbitrary type are given unique IDs.
class idm
{
	template <typename T>
	static inline u64 get_type()
	{
		return id_manager::typeinfo::get_index<T>();
	}

	template <typename T>
	static constexpr u32 get_index(u32 id)
	{
		using traits = id_manager::id_traits<T>;

		return id_manager::get_index(id, traits::base, traits::step, traits::count, traits::invl_range);
	}

	// Helper
	template <typename F>
	struct function_traits;

	template <typename F, typename R, typename A1, typename A2>
	struct function_traits<R (F::*)(A1, A2&) const>
	{
		using object_type = A2;
		using result_type = R;
	};

	template <typename F, typename R, typename A1, typename A2>
	struct function_traits<R (F::*)(A1, A2&)>
	{
		using object_type = A2;
		using result_type = R;
	};

	template <typename F, typename A1, typename A2>
	struct function_traits<void (F::*)(A1, A2&) const>
	{
		using object_type = A2;
		using void_type   = void;
	};

	template <typename F, typename A1, typename A2>
	struct function_traits<void (F::*)(A1, A2&)>
	{
		using object_type = A2;
		using void_type   = void;
	};

	// Helper type: pointer + return value propagated
	template <typename T, typename RT>
	struct return_pair
	{
		std::shared_ptr<T> ptr;
		RT ret;

		explicit operator bool() const
		{
			return ptr.operator bool();
		}

		T& operator*() const
		{
			return *ptr;
		}

		T* operator->() const
		{
			return ptr.get();
		}
	};

	// Unsafe specialization (not refcounted)
	template <typename T, typename RT>
	struct return_pair<T*, RT>
	{
		T* ptr;
		RT ret;

		explicit operator bool() const
		{
			return ptr != nullptr;
		}

		T& operator*() const
		{
			return *ptr;
		}

		T* operator->() const
		{
			return ptr;
		}
	};

	using map_data = std::pair<id_manager::id_key, std::shared_ptr<void>>;

	// Prepare new ID (returns nullptr if out of resources)
	static map_data* allocate_id(std::vector<map_data>& vec, u64 type_id, u32 dst_id, u32 base, u32 step, u32 count, std::pair<u32, u32> invl_range);

	// Find ID (additionally check type if types are not equal)
	template <typename T, typename Type>
	static map_data* find_id(u32 id)
	{
		static_assert(id_manager::id_verify<T, Type>::value, "Invalid ID type combination");

		const u32 index = get_index<Type>(id);

		if (index >= id_manager::id_traits<Type>::count)
		{
			return nullptr;
		}

		auto& vec = g_fxo->get<id_manager::id_map<T>>().vec;

		if (index >= vec.size())
		{
			return nullptr;
		}

		auto& data = vec[index];

		if (data.second)
		{
			if (std::is_same<T, Type>::value || data.first.type() == get_type<Type>())
			{
				if (!id_manager::id_traits<Type>::invl_range.second || data.first.value() == id)
				{
					return &data;
				}
			}
		}

		return nullptr;
	}

	// Allocate new ID (or use fixed ID) and assign the object from the provider()
	template <typename T, typename Type, typename F>
	static map_data* create_id(F&& provider, u32 id = id_manager::id_traits<Type>::invalid)
	{
		static_assert(id_manager::id_verify<T, Type>::value, "Invalid ID type combination");

		// ID traits
		using traits = id_manager::id_traits<Type>;

		// Allocate new id
		std::lock_guard lock(id_manager::g_mutex);

		auto& map = g_fxo->get<id_manager::id_map<T>>();

		if (auto* place = allocate_id(map.vec, get_type<Type>(), id, traits::base, traits::step, traits::count, traits::invl_range))
		{
			// Get object, store it
			place->second = provider();

			if (place->second)
			{
				return place;
			}
		}

		return nullptr;
	}

public:

	// Remove all objects of a type
	template <typename T>
	static inline void clear()
	{
		std::lock_guard lock(id_manager::g_mutex);
		g_fxo->get<id_manager::id_map<T>>().vec.clear();
	}

	// Get last ID (updated in create_id/allocate_id)
	static inline u32 last_id()
	{
		return id_manager::g_id;
	}

	// Add a new ID of specified type with specified constructor arguments (returns object or nullptr)
	template <typename T, typename Make = T, typename... Args>
	static inline std::enable_if_t<std::is_constructible<Make, Args...>::value, std::shared_ptr<Make>> make_ptr(Args&&... args)
	{
		if (auto pair = create_id<T, Make>([&] { return std::make_shared<Make>(std::forward<Args>(args)...); }))
		{
			return {pair->second, static_cast<Make*>(pair->second.get())};
		}

		return nullptr;
	}

	// Add a new ID of specified type with specified constructor arguments (returns id)
	template <typename T, typename Make = T, typename... Args>
	static inline std::enable_if_t<std::is_constructible<Make, Args...>::value, u32> make(Args&&... args)
	{
		if (auto pair = create_id<T, Make>([&] { return std::make_shared<Make>(std::forward<Args>(args)...); }))
		{
			return pair->first;
		}

		return id_manager::id_traits<Make>::invalid;
	}

	// Add a new ID for an existing object provided (returns new id)
	template <typename T, typename Made = T>
	static inline u32 import_existing(const std::shared_ptr<T>& ptr, u32 id = id_manager::id_traits<Made>::invalid)
	{
		if (auto pair = create_id<T, Made>([&] { return ptr; }, id))
		{
			return pair->first;
		}

		return id_manager::id_traits<Made>::invalid;
	}

	// Add a new ID for an object returned by provider()
	template <typename T, typename Made = T, typename F, typename = std::invoke_result_t<F>>
	static inline u32 import(F&& provider)
	{
		if (auto pair = create_id<T, Made>(std::forward<F>(provider)))
		{
			return pair->first;
		}

		return id_manager::id_traits<Made>::invalid;
	}

	// Access the ID record without locking (unsafe)
	template <typename T, typename Get = T>
	static inline map_data* find_unlocked(u32 id)
	{
		return find_id<T, Get>(id);
	}

	// Check the ID without locking (can be called from other method)
	template <typename T, typename Get = T>
	static inline Get* check_unlocked(u32 id)
	{
		if (const auto found = find_id<T, Get>(id))
		{
			return static_cast<Get*>(found->second.get());
		}

		return nullptr;
	}

	// Check the ID
	template <typename T, typename Get = T>
	static inline Get* check(u32 id)
	{
		reader_lock lock(id_manager::g_mutex);

		return check_unlocked<T, Get>(id);
	}

	// Check the ID, access object under shared lock
	template <typename T, typename Get = T, typename F, typename FRT = std::invoke_result_t<F, Get&>>
	static inline auto check(u32 id, F&& func)
	{
		reader_lock lock(id_manager::g_mutex);

		if (const auto ptr = check_unlocked<T, Get>(id))
		{
			if constexpr (!std::is_void_v<FRT>)
			{
				return return_pair<Get*, FRT>{ptr, func(*ptr)};
			}
			else
			{
				func(*ptr);
				return ptr;
			}
		}

		if constexpr (!std::is_void_v<FRT>)
		{
			return return_pair<Get*, FRT>{nullptr};
		}
		else
		{
			return static_cast<Get*>(nullptr);
		}
	}

	// Get the object without locking (can be called from other method)
	template <typename T, typename Get = T>
	static inline std::shared_ptr<Get> get_unlocked(u32 id)
	{
		const auto found = find_id<T, Get>(id);

		if (found == nullptr) [[unlikely]]
		{
			return nullptr;
		}

		return std::static_pointer_cast<Get>(found->second);
	}

	// Get the object
	template <typename T, typename Get = T>
	static inline std::shared_ptr<Get> get(u32 id)
	{
		reader_lock lock(id_manager::g_mutex);

		const auto found = find_id<T, Get>(id);

		if (found == nullptr) [[unlikely]]
		{
			return nullptr;
		}

		return std::static_pointer_cast<Get>(found->second);
	}

	// Get the object, access object under reader lock
	template <typename T, typename Get = T, typename F, typename FRT = std::invoke_result_t<F, Get&>>
	static inline std::conditional_t<std::is_void_v<FRT>, std::shared_ptr<Get>, return_pair<Get, FRT>> get(u32 id, F&& func)
	{
		reader_lock lock(id_manager::g_mutex);

		const auto found = find_id<T, Get>(id);

		if (found == nullptr) [[unlikely]]
		{
			return {nullptr};
		}

		const auto ptr = static_cast<Get*>(found->second.get());

		if constexpr (std::is_void_v<FRT>)
		{
			func(*ptr);
			return {found->second, ptr};
		}
		else
		{
			return {{found->second, ptr}, func(*ptr)};
		}
	}

	// Access all objects of specified type. Returns the number of objects processed.
	template <typename T, typename Get = T, typename F, typename FT = decltype(&std::decay_t<F>::operator()), typename FRT = typename function_traits<FT>::void_type>
	static inline u32 select(F&& func, int = 0)
	{
		static_assert(id_manager::id_verify<T, Get>::value, "Invalid ID type combination");

		reader_lock lock(id_manager::g_mutex);

		u32 result = 0;

		for (auto& id : g_fxo->get<id_manager::id_map<T>>().vec)
		{
			if (id.second)
			{
				if (std::is_same<T, Get>::value || id.first.type() == get_type<Get>())
				{
					func(id.first, *static_cast<typename function_traits<FT>::object_type*>(id.second.get()));
					result++;
				}
			}
		}

		return result;
	}

	// Access all objects of specified type. If function result evaluates to true, stop and return the object and the value.
	template <typename T, typename Get = T, typename F, typename FT = decltype(&std::decay_t<F>::operator()), typename FRT = typename function_traits<FT>::result_type>
	static inline auto select(F&& func)
	{
		static_assert(id_manager::id_verify<T, Get>::value, "Invalid ID type combination");

		using object_type = typename function_traits<FT>::object_type;
		using result_type = return_pair<object_type, FRT>;

		reader_lock lock(id_manager::g_mutex);

		for (auto& id : g_fxo->get<id_manager::id_map<T>>().vec)
		{
			if (auto ptr = static_cast<object_type*>(id.second.get()))
			{
				if (std::is_same<T, Get>::value || id.first.type() == get_type<Get>())
				{
					if (FRT result = func(id.first, *ptr))
					{
						return result_type{{id.second, ptr}, std::move(result)};
					}
				}
			}
		}

		return result_type{nullptr};
	}

	// Remove the ID
	template <typename T, typename Get = T>
	static inline bool remove(u32 id)
	{
		std::shared_ptr<void> ptr;
		{
			std::lock_guard lock(id_manager::g_mutex);

			if (const auto found = find_id<T, Get>(id))
			{
				ptr = std::move(found->second);
			}
			else
			{
				return false;
			}
		}

		return true;
	}

	// Remove the ID if matches the weak/shared ptr
	template <typename T, typename Get = T, typename Ptr>
	static inline bool remove_verify(u32 id, Ptr sptr)
	{
		std::shared_ptr<void> ptr;
		{
			std::lock_guard lock(id_manager::g_mutex);

			if (const auto found = find_id<T, Get>(id); found &&
				(!found->second.owner_before(sptr) && !sptr.owner_before(found->second)))
			{
				ptr = std::move(found->second);
			}
			else
			{
				return false;
			}
		}

		return true;
	}

	// Remove the ID and return the object
	template <typename T, typename Get = T>
	static inline std::shared_ptr<Get> withdraw(u32 id)
	{
		std::shared_ptr<Get> ptr;
		{
			std::lock_guard lock(id_manager::g_mutex);

			if (const auto found = find_id<T, Get>(id))
			{
				ptr = std::static_pointer_cast<Get>(::as_rvalue(std::move(found->second)));
			}
		}

		return ptr;
	}

	// Remove the ID after accessing the object under writer lock, return the object and propagate return value
	template <typename T, typename Get = T, typename F, typename FRT = std::invoke_result_t<F, Get&>>
	static inline std::conditional_t<std::is_void_v<FRT>, std::shared_ptr<Get>, return_pair<Get, FRT>> withdraw(u32 id, F&& func)
	{
		std::unique_lock lock(id_manager::g_mutex);

		if (const auto found = find_id<T, Get>(id))
		{
			const auto _ptr = static_cast<Get*>(found->second.get());

			if constexpr (std::is_void_v<FRT>)
			{
				func(*_ptr);
				return std::static_pointer_cast<Get>(::as_rvalue(std::move(found->second)));
			}
			else
			{
				FRT ret = func(*_ptr);

				if (ret)
				{
					// If return value evaluates to true, don't delete the object (error code)
					return {{found->second, _ptr}, std::move(ret)};
				}

				return {std::static_pointer_cast<Get>(::as_rvalue(std::move(found->second))), std::move(ret)};
			}
		}

		return {nullptr};
	}
};
