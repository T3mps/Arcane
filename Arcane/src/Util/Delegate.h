#pragma once

namespace ARC
{
	template <typename T>
	class Delegate;

	template<typename ReturnType, typename... ArgsType>
	class Delegate<ReturnType(ArgsType...)>
	{
		using InternalFunction = ReturnType(*)(void*, ArgsType...);

		struct InvocationElement
		{
			InvocationElement() = default;
			InvocationElement(void* thisPtr, InternalFunction aStub) : object(thisPtr), stub(aStub) {}

			bool operator ==(const InvocationElement& another) const { return another.stub == stub && another.object == object; }
			bool operator !=(const InvocationElement& another) const { return another.stub != stub || another.object != object; }

			void* object = nullptr;
			InternalFunction stub = nullptr;
		};

	public:
		Delegate() = default;
		Delegate(const Delegate& other) { m_invocation = other.m_invocation; }

		Delegate& operator =(const Delegate& other) { m_invocation = other.m_invocation; return *this; }
		
		bool operator == (const Delegate& other) const { return m_invocation == other.m_invocation; }
		bool operator != (const Delegate& other) const { return m_invocation != other.m_invocation; }

		ReturnType operator()(ArgsType... args) { Invoke(args...); }

		template<ReturnType(*Function)(ArgsType...)>
		void Bind()
		{
			Assign(nullptr, FreeFunctionStub<Function>);
		}
		
		template<typename Lambda>
		void BindLambda(const Lambda& lambda)
		{
			using LambdaType = std::remove_cvref_t<Lambda>;
			Assign((void*)(&lambda), LambdaStub<LambdaType>);
		}

		template<typename Function, typename Class>
			requires std::is_member_function_pointer_v<Function> && std::is_same_v<Function, ReturnType(Class::*)(ArgsType...)>
		void Bind(Class* object)
		{
			Assign(static_cast<void*>(object), MemberFunctionStub<Class, Function>);
		}

		template<typename Function, typename Class>
			requires std::is_member_function_pointer_v<Function> && std::is_same_v<Function, ReturnType(Class::*)(ArgsType...) const>
		void Bind(const Class* object)
		{
			Assign(const_cast<void*>(static_cast<const void*>(object)), ConstMemberFunctionStub<Class, Function>);
		}

		void Unbind()
		{
			m_invocation = InvocationElement();
		}

		bool IsBound() const { return m_invocation.stub; }
		operator bool() const { return IsBound(); }

		ReturnType Invoke(ArgsType...args) const
		{
			ARC_ASSERT(IsBound(), "Trying to invoke unbound delegate.");
			return std::invoke(m_invocation.stub, m_invocation.object, std::forward<ArgsType>(args)...);
		}

	private:
		inline void Assign(void* anObject, InternalFunction aStub)
		{
			m_invocation.object = anObject;
			m_invocation.stub = aStub;
		}

		template <typename Class, ReturnType(Class::* Function)(ArgsType...)>
		static constexpr ReturnType MemberFunctionStub(void* thisPtr, ArgsType... args)
		{
			Class* p = static_cast<Class*>(thisPtr);
			return (p->*Function)(std::forward<ArgsType>(args)...);
		}

		template <typename Class, ReturnType(Class::* Function)(ArgsType...) const>
		static constexpr ReturnType ConstMemberFunctionStub(void* thisPtr, ArgsType... args)
		{
			const Class* p = static_cast<Class*>(thisPtr);
			return (p->*Function)(std::forward<ArgsType>(args)...);
		}

		template<ReturnType(*Function)(ArgsType...)>
		static ReturnType FreeFunctionStub(void* thisPtr, ArgsType... args)
		{
			return (Function)(std::forward<ArgsType>(args)...);
		}

		template <typename Lambda>
		static ReturnType LambdaStub(void* thisPtr, ArgsType... args)
		{
			Lambda* p = static_cast<Lambda*>(thisPtr);
			return (p->operator())(std::forward<ArgsType>(args)...);
		}

	private:
		InvocationElement m_invocation;
	};
} // namespace ARC
