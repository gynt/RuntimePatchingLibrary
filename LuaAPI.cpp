
#include "framework.h"
#include <iostream>
#include <sstream>
#include <set>
#include <map>
#include <assert.h>
#include <vector>
#include "AOB.h"

#include "LuaAPI.h"

namespace LuaAPI {

	constexpr INT8 NOP = (INT8)0x90;
	constexpr INT8 JMP = (INT8)0xE9;
	constexpr INT8 CALL = (INT8)0xE8;

	void LuaLandingFromCpp();
	void detourLandingFunction();

	int CALLING_CONV_CALLER = 0; //add esp, 4*argumentCount
	int CALLING_CONV_THISCALL = 1; //mov ecx, address
	int CALLING_CONV_STDCALL = 2; //special ret 0x, no this parameter
	//int CALLING_CONV_FASTCALL = 2; ?

	HANDLE heap;

	bool DoCreateCallHook(DWORD from_address, DWORD to_address, int hookSize, DWORD& newFunctionLocation) {
		int size = hookSize;
		if (size < 5) return FALSE;

		BYTE* fun_o_ptr = (BYTE*)from_address;
		BYTE* fun_h_ptr = (BYTE*)to_address;

		// create gateway
		BYTE* gateway = (BYTE*)VirtualAlloc(0, size, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
		memcpy_s(gateway, size, fun_o_ptr, size);
		uintptr_t gatewayRelAddress = fun_o_ptr - gateway - 5;

		*(gateway + size) = JMP;
		*(uintptr_t*)((uintptr_t)gateway + size + 1) = gatewayRelAddress;

		// detour

		DWORD oldProtect;
		VirtualProtect(fun_o_ptr, size, PAGE_EXECUTE_READWRITE, &oldProtect);

		memset(fun_o_ptr, NOP, size); // needs to be done, otherwise this confuses the CE disassmbler

		uintptr_t relAddress = fun_h_ptr - fun_o_ptr - 5;

		*fun_o_ptr = CALL;
		*(uintptr_t*)(fun_o_ptr + 1) = relAddress;

		VirtualProtect(fun_o_ptr, size, oldProtect, &oldProtect);

		// point original function to gateway
		newFunctionLocation = (DWORD)& gateway[0];

		return TRUE;
	}

	lua_State* L;

	class LuaHook {
	public:
		DWORD address;
		int hookSize;
		int callingConvention;
		int argumentsCount;
		DWORD newOriginalFunctionLocation;
		std::string luaHookFunctionName;
		std::string luaOriginalFunctionName;
		DWORD thisValue;

		LuaHook(DWORD addr, int hookSize, int callingConv, int argCount, std::string luaHook, std::string luaOriginal) {
			this->address = addr;
			this->hookSize = hookSize;
			this->callingConvention = callingConv;
			this->argumentsCount = argCount - (int)(callingConv == 1); // To keep it clear for the lua users...
			this->luaHookFunctionName = luaHook;
			this->luaOriginalFunctionName = luaOriginal;
		}

		bool CreateCallHook() {
			return DoCreateCallHook(this->address, (DWORD)LuaLandingFromCpp, this->hookSize, this->newOriginalFunctionLocation);
		}

		void registerOriginalFunctionInLua() {
			std::stringstream f("");
			f << "function " << this->luaOriginalFunctionName << "(";
			if (this->callingConvention == 1) {
				f << "this";
			}
			for (int i = 0; i < this->argumentsCount; i++) {
				if (i != 0 || this->callingConvention == 1) {
					f << ", ";
				}
				f << "arg" << i;
			}
			f << ")" << std::endl;
			//f << "print('calling original c++ function at new location: " << this->newOriginalFunctionLocation << "')" << std::endl;
			f << "return callOriginal(" << this->address;
			if (this->callingConvention == 1) {
				f << ", this";
			}
			if (this->argumentsCount > 0) {
				for (int i = 0; i < this->argumentsCount; i++) {
					//if (i != 0) { // no need for this because we prove the first argument.
					f << ", ";
					//}
					f << "arg" << std::dec << i;
				}
			}

			f << ")" << std::endl;
			f << "end" << std::endl;

			int result = luaL_dostring(L, f.str().c_str());
			if (result != LUA_OK) {
				std::cout << "[LUA API]: ERROR in registering function: " << lua_tostring(L, -1) << std::endl;
				std::cout << "function that was going to be registered:" << std::endl << f.str() << std::endl;
				lua_pop(L, 1); // pop off the error message;
			}
		}
	};

	std::map<DWORD, std::shared_ptr<LuaHook>> hookMapping;

	char luaHookedFunctionName[100];
	DWORD newOriginalFunctionLocation;
	DWORD functionLocation;
	int luaHookedFunctionArgCount;
	int luaErrorLevel;
	std::string luaErrorMsg;
	int luaCallingConvention;
	DWORD currentECXValue;

	// lua calls this function as: hookCode(luaOriginal = "callback", address = 0xDEADBEEF, argumentCount = 3, callingConvention = 0)
	int luaExposeCode(lua_State* L) {
		if (lua_gettop(L) != 4) {
			return luaL_error(L, "expecting exactly 4 arguments");
		}

		std::string luaOriginal = lua_tostring(L, 1);
		DWORD address = lua_tointeger(L, 2);
		int argumentCount = lua_tointeger(L, 3);
		int callingConvention = lua_tointeger(L, 4);
		//DWORD thisValue = lua_tointeger(L, 5);

		hookMapping.insert(std::pair<DWORD, std::shared_ptr<LuaHook>>(address, std::make_shared<LuaHook>(address, 0, callingConvention, argumentCount, "", luaOriginal)));
		hookMapping[address]->newOriginalFunctionLocation = address;
		hookMapping[address]->registerOriginalFunctionInLua();

		return 0;
	}

	// lua calls this function as: hookCode(luaHook = "callbackTest", luaOriginal = "callback", address = 0xDEADBEEF, argumentCount = 3, callingConvention = 0, hookSize = 5)
	int luaHookCode(lua_State* L) {

		if (lua_gettop(L) != 6) {
			return luaL_error(L, "expecting exactly 6 arguments");
		}

		std::string luaHook = lua_tostring(L, 1);
		std::string luaOriginal = lua_tostring(L, 2);
		DWORD address = lua_tointeger(L, 3);
		int argumentCount = lua_tointeger(L, 4);
		int callingConvention = lua_tointeger(L, 5);
		//DWORD thisValue = lua_tointeger(L, 6);
		int hookSize = lua_tointeger(L, 6);

		hookMapping.insert(std::pair<DWORD, std::shared_ptr<LuaHook>>(address, std::make_shared<LuaHook>(address, hookSize, callingConvention, argumentCount, luaHook, luaOriginal)));

		hookMapping[address]->CreateCallHook();
		hookMapping[address]->registerOriginalFunctionInLua();

		return 1;
	}

	DWORD __stdcall executeLuaHook(unsigned long* args) {
		lua_getglobal(L, luaHookedFunctionName);
		if (lua_isfunction(L, -1)) {
			int totalArgCount = luaHookedFunctionArgCount;
			if (luaCallingConvention == 1) {
				lua_pushnumber(L, currentECXValue);
				totalArgCount += 1;
			}
			for (int i = 0; i < luaHookedFunctionArgCount; i++) {
				lua_pushnumber(L, args[i]);
			}
			if (lua_pcall(L, totalArgCount, 1, 0) == LUA_OK) {
				luaErrorLevel = 0;
				luaErrorMsg = "";
				int ret = (DWORD)lua_tonumber(L, -1);
				lua_pop(L, 1); // pop off the return value;
				return ret;
			}
			else {
				luaErrorLevel = 1;
				luaErrorMsg = lua_tostring(L, -1);
				lua_pop(L, 1); // pop off the error message;
			}
		}
		else {
			lua_pop(L, 1); // I think we need this pop, because the getglobal does a push that would otherwise be popped by pcall.
			luaErrorLevel = 2;
			luaErrorMsg = std::string(luaHookedFunctionName) + " is not a function";
		}

		std::cout << "[LUA API]: " << std::string(luaErrorMsg) << std::endl;
		return 0;
	}

	void __stdcall SetLuaHookedFunctionParameters(DWORD origin, DWORD liveECXValue) {
		std::map<DWORD, std::shared_ptr<LuaHook>>::iterator it;

		it = hookMapping.find(origin);
		if (it != hookMapping.end()) {
			std::shared_ptr<LuaHook> value = it->second;
			memset(luaHookedFunctionName, 0, 100);
			memcpy(luaHookedFunctionName, value->luaHookFunctionName.c_str(), value->luaHookFunctionName.size());
			luaHookedFunctionArgCount = value->argumentsCount;
			luaCallingConvention = value->callingConvention;
			newOriginalFunctionLocation = value->newOriginalFunctionLocation;
			functionLocation = value->address;
			currentECXValue = liveECXValue;
		}
		else {

		}

	}

	DWORD fakeStack[20];

	// The user has called the luaOriginalFunctionName
	int cppCallCode(lua_State* L) {
		DWORD address = lua_tointeger(L, 1);

		SetLuaHookedFunctionParameters(address, 0);

		if (luaCallingConvention == 1) {
			int totalArgCount = luaHookedFunctionArgCount + 1;
			if (lua_gettop(L) != totalArgCount + 1) {
				std::cout << "[LUA API]: calling function " << std::hex << functionLocation << " with too few arguments;" << std::endl;
				return luaL_error(L, ("[LUA API]: calling function " + std::to_string(functionLocation) + " with too few arguments;").c_str());
			}

			for (int i = 0; i < luaHookedFunctionArgCount; i++) {
				fakeStack[i] = lua_tointeger(L, i + 2 + 1);
			}

			currentECXValue = lua_tointeger(L, 2);
		}
		else {
			if (lua_gettop(L) != luaHookedFunctionArgCount + 1) {
				std::cout << "[LUA API]: calling function " << std::hex << functionLocation << " with too few arguments;" << std::endl;
				return luaL_error(L, ("[LUA API]: calling function " + std::to_string(functionLocation) + " with too few arguments;").c_str());
			}

			for (int i = 0; i < luaHookedFunctionArgCount; i++) {
				fakeStack[i] = lua_tointeger(L, i + 2);
			}


		}

		__asm {
			mov ecx, luaHookedFunctionArgCount;
		loopbegin:
			cmp ecx, 0;
			jle done;
			dec ecx;
			mov eax, fakeStack[ecx * 4];
			push eax;
			jmp loopbegin;
		done:
			mov ecx, luaCallingConvention;// FAIL! this parameter is not valid anymore at this point!
			cmp ecx, 0;
			je caller;
			jmp callee;
		caller:
			mov ecx, luaHookedFunctionArgCount;
			mov eax, newOriginalFunctionLocation;
			cmp ecx, 0;
			je add0x00;
			cmp ecx, 1;
			je add0x04;
			cmp ecx, 2;
			je add0x08;
			cmp ecx, 3;
			je add0x0C;
			cmp ecx, 4;
			je add0x10;
			cmp ecx, 5;
			je add0x14;
			cmp ecx, 6;
			je add0x18;
			cmp ecx, 7;
			je add0x1C;
			cmp ecx, 8;
			je add0x20;
		add0x00:
			call eax;
			add esp, 0x00;
			jmp eor;
		add0x04:
			call eax;
			add esp, 0x04;
			jmp eor;
		add0x08:
			call eax;
			add esp, 0x08;
			jmp eor;
		add0x0C:
			call eax;
			add esp, 0x0C;
			jmp eor;
		add0x10:
			call eax;
			add esp, 0x10;
			jmp eor;
		add0x14:
			call eax;
			add esp, 0x14;
			jmp eor;
		add0x18:
			call eax;
			add esp, 0x18;
			jmp eor;
		add0x1c:
			call eax;
			add esp, 0x1c;
			jmp eor;
		add0x20:
			call eax;
			add esp, 0x20;
			jmp eor;
		callee:
			mov eax, newOriginalFunctionLocation;
			mov ecx, currentECXValue;
			call eax;
		eor:
		}

		DWORD result;
		__asm {
			mov result, eax;
		}
		lua_pushinteger(L, result);

		return 1;
	}

	void __declspec(naked) LuaLandingFromCpp() {
		__asm {
			// pop the previous EIP+5 into eax
			pop eax;
			push ecx;
			sub eax, 5;
			push eax;
			// set up the right global variables for the current hook
			call SetLuaHookedFunctionParameters;

			//At this point, we should have the original call stack
			mov eax, esp;
			// compensate for the return address we have on the stack...
			add eax, 4;
			push eax;

			mov eax, [luaCallingConvention];
			cmp eax, 0;
			je retNone;
			cmp eax, 1; // this cmp is bullshit for now, because calling convention 0 is the only with caller cleanup.
			mov eax, [luaHookedFunctionArgCount];
			cmp eax, 0;
			je ret0x0;
			cmp eax, 1;
			je ret0x4;
			cmp eax, 2;
			je ret0x8;
			cmp eax, 3;
			je ret0xC;
			cmp eax, 4;
			je ret0x10;
			cmp eax, 5;
			je ret0x14;
			cmp eax, 6;
			je ret0x18;
			cmp eax, 7;
			je ret0x1C;
			cmp eax, 8;
			je ret0x20;
			jmp retNone;

		ret0x0:
			call executeLuaHook;
			ret 0x0;

		ret0x4:
			call executeLuaHook;
			ret 0x4;

		ret0x8:
			call executeLuaHook;
			ret 0x8;

		ret0xC:
			call executeLuaHook;
			ret 0xC;

		ret0x10:
			call executeLuaHook;
			ret 0x10;

		ret0x14:
			call executeLuaHook;
			ret 0x14;

		ret0x18:
			call executeLuaHook;
			ret 0x18;

		ret0x1C:
			call executeLuaHook;
			ret 0x1C;

		ret0x20:
			call executeLuaHook;
			ret 0x20;

		retNone:
			call executeLuaHook;
			ret;
		}
	}

	std::map<DWORD, std::pair<std::string, DWORD>> detourTargetMap;
	DWORD currentDetourSource;
	DWORD currentDetourReturn;
	std::string currentDetourTarget;

	// lua calls this as: detourCode(hookedFunctionName, address, hookSize)
	int luaDetourCode(lua_State* L) {
		if (lua_gettop(L) != 3) {
			return luaL_error(L, "expecting exactly 3 arguments");
		}

		std::string luaOriginal = lua_tostring(L, 1);
		DWORD address = lua_tointeger(L, 2);
		int hookSize = lua_tointeger(L, 3);

		DWORD ret;
		DoCreateCallHook(address, (DWORD)detourLandingFunction, hookSize, ret);
		detourTargetMap[address] = std::pair<std::string, DWORD>(luaOriginal, ret);
		return 0;
	}

	void __stdcall GetDetourLuaTargetAndCallTheLuaFunction(DWORD address, DWORD* registers) {
		bool exists = detourTargetMap.count(address) == 1;
		if (!exists) {
			assert(exists);
		}
		std::pair<std::string, DWORD> entry = detourTargetMap[address];
		std::string luaFunction = entry.first;
		DWORD retLoc = entry.second;
		currentDetourReturn = retLoc;

		const std::vector<std::string> order = { "EDI", "ESI", "EBP", "ESP", "EBX", "EDX", "ECX", "EAX" };

		lua_getglobal(L, luaFunction.c_str());
		if (lua_isfunction(L, -1)) {
			lua_createtable(L, 0, 8);

			for (int i = 0; i < order.size(); i++) {
				lua_pushstring(L, order[i].c_str());
				lua_pushinteger(L, registers[i]);
				lua_settable(L, -3);  /* 3rd element from the stack top */
			}

			// We call the function and pass 1 argument and expect 1 argument in return.
			if (lua_pcall(L, 1, 1, 0) == LUA_OK) {
				luaErrorLevel = 0;
				luaErrorMsg = "";
				if (lua_istable(L, -1)) {
					/* table is in the stack at index 't' */
					lua_pushnil(L);  /* first key */
					while (lua_next(L, -2) != 0) {
						/* uses 'key' (at index -2) and 'value' (at index -1) */
						if (!lua_isstring(L, -2)) {
							lua_pushliteral(L, "The return value table must have string keys");
							lua_error(L);
							currentDetourReturn = retLoc;
							return;
						}
						if (!lua_isinteger(L, -1)) {
							lua_pushliteral(L, "The return value table must have integer values");
							lua_error(L);
							currentDetourReturn = retLoc;
							return;
						}

						std::string key = lua_tostring(L, -2);
						DWORD value = lua_tointeger(L, -1);

						std::vector<std::string>::const_iterator it = find(order.begin(), order.end(), key);
						if (it == order.end()) {
							lua_pushstring(L, ("The key does not exist: " + key).c_str());
							lua_error(L);
							currentDetourReturn = retLoc;
							return;
						}

						int index = it - order.begin();
						registers[index] = value;

						/* removes 'value'; keeps 'key' for next iteration */
						lua_pop(L, 1);
					}
					currentDetourReturn = retLoc;
					return;
				}
				else {
					luaErrorLevel = 3;
					luaErrorMsg = std::string(luaHookedFunctionName) + " is not a function";
					lua_pushstring(L, luaErrorMsg.c_str());
					lua_error(L);
				}
				lua_pop(L, 1); // pop off the return value;
				currentDetourReturn = retLoc;
				return;
			}
			else {
				luaErrorLevel = 1;
				luaErrorMsg = lua_tostring(L, -1);
				lua_pushstring(L, luaErrorMsg.c_str());
				lua_error(L);
				lua_pop(L, 1); // pop off the error message;
			}
		}
		else {
			lua_pop(L, 1); // I think we need this pop, because the getglobal does a push that would otherwise be popped by pcall.
			luaErrorLevel = 2;
			luaErrorMsg = std::string(luaHookedFunctionName) + " is not a function";
			lua_pushstring(L, luaErrorMsg.c_str());
			lua_error(L);
		}

		std::cout << "[LUA API]: " << std::string(luaErrorMsg) << std::endl;
		currentDetourReturn = retLoc;
	}

	void __declspec(naked) detourLandingFunction() {
		__asm {

			pushfd; // pushes 1 element
			pushad; // pushes 8 elements

			mov ecx, esp; // store a pointer to the register values on the stack.

			mov eax, [esp + (9 * 0x04)]; // the 9th element will be the return address from the detour.
			sub eax, 5; // subtract 5 because a jump is 5 long to get the origin address.
			push ecx; // push the register array;
			push eax; // set this as an argument to the function.

			// set up the right global variables for the current detour
			call GetDetourLuaTargetAndCallTheLuaFunction; // this function also should set currentDetourReturn;

			popad;
			popfd;

			add esp, 4; // remove the address that was pushed by the call detour

			jmp currentDetourReturn;
		}
	}

	static int l_my_print(lua_State* L) {
		int nargs = lua_gettop(L);
		std::cout << "[LUA]: ";
		for (int i = 1; i <= nargs; ++i) {
			bool isNil = lua_isnil(L, i);
			if (isNil) {
				std::cout << "ERROR: cannot print a nil value";
				return luaL_error(L, "cannot print a nil value");
			}
			else if (lua_istable(L, i)) {
				std::cout << "[object]";
			}
			else {
				std::cout << lua_tostring(L, i);
			}
		}
		std::cout << std::endl;

		return 0;
	}

	static const struct luaL_Reg printlib[] = {
	  {"print", l_my_print},
	  {NULL, NULL} /* end of array */
	};


	int luaReadByte(lua_State* L) {
		if (lua_gettop(L) != 1) {
			return luaL_error(L, "expected exactly 1 argument");
		}
		DWORD address = lua_tointeger(L, 1);
		lua_pushinteger(L, *((BYTE*)address));
		return 1;
	}

	int luaReadSmallInteger(lua_State* L) {
		if (lua_gettop(L) != 1) {
			return luaL_error(L, "expected exactly 1 argument");
		}
		DWORD address = lua_tointeger(L, 1);
		lua_pushinteger(L, *((SHORT*)address));
		return 1;
	}

	int luaReadInteger(lua_State* L) {
		if (lua_gettop(L) != 1) {
			return luaL_error(L, "expected exactly 1 argument");
		}
		DWORD address = lua_tointeger(L, 1);
		lua_pushinteger(L, *((int*)address));
		return 1;
	}

	int luaReadString(lua_State* L) {
		DWORD address = 0;
		int maxLength = 0;
		int length = 0;
		bool wide = false;
		if (lua_gettop(L) == 0) {
			return luaL_error(L, "too few arguments passed to readString");
		}
		address = lua_tointeger(L, 1);
		if (lua_gettop(L) == 2) {
			maxLength = lua_tointeger(L, 2);
		}
		if (lua_gettop(L) == 3) {
			wide = lua_tointeger(L, 3) == 1;
		}

		if (wide || maxLength) {
			return luaL_error(L, "sorry, maxlength and wide are not supported yet.");
		}

		std::string result((char*)address);
		lua_pushstring(L, result.c_str());

		return 1;
	}

	int luaReadBytes(lua_State* L) {
		if (lua_gettop(L) != 2) {
			return luaL_error(L, "expected exactly 2 arguments");
		}

		DWORD address = lua_tointeger(L, 1);
		int size = lua_tointeger(L, 2);

		lua_createtable(L, size, 0);

		for (int i = 0; i < size; i++) {
			unsigned char value = *((BYTE*)(address + i));
			lua_pushinteger(L, (lua_Integer)i + 1);
			lua_pushinteger(L, value);
			lua_settable(L, -3);  /* 3rd element from the stack top */
		}

		// we pass the table back;

		return 1;
	}

	int luaWriteByte(lua_State* L) {
		if (lua_gettop(L) != 2) {
			return luaL_error(L, "expected exactly 2 arguments");
		}
		DWORD address = lua_tointeger(L, 1);
		BYTE value = lua_tointeger(L, 2);
		*((BYTE*)address) = value;
		return 0;
	}

	int luaWriteSmallInteger(lua_State* L) {
		if (lua_gettop(L) != 2) {
			return luaL_error(L, "expected exactly 2 arguments");
		}
		DWORD address = lua_tointeger(L, 1);
		SHORT value = lua_tointeger(L, 2);
		*((SHORT*)address) = value;
		return 0;
	}

	int luaWriteInteger(lua_State* L) {
		if (lua_gettop(L) != 2) {
			return luaL_error(L, "expected exactly 2 arguments");
		}
		DWORD address = lua_tointeger(L, 1);
		int value = lua_tointeger(L, 2);
		*((int*)address) = value;
		return 0;
	}

	int luaWriteBytes(lua_State* L) {
		if (lua_gettop(L) != 2) {
			return luaL_error(L, "expected exactly 2 arguments");
		}
		DWORD address = lua_tointeger(L, 1);
		if (!lua_istable(L, 2)) {
			return luaL_error(L, "the second argument should be a table");
		}

		int i = 0;

		lua_pushvalue(L, 2); // push the table again; so that it is at -1

		/* table is in the stack at index 't' */
		lua_pushnil(L);  /* first key */
		while (lua_next(L, -2) != 0) {
			/* uses 'key' (at index -2) and 'value' (at index -1) */
			if (!lua_isinteger(L, -1)) {
				//lua_pushliteral(L, "The return value table must have integer values");
				//lua_error(L);
				return luaL_error(L, "The return value table must have integer values");
			}

			int value = lua_tointeger(L, -1);
			if (value < 0) {
				return luaL_error(L, "The values must all be positive");
			}

			*((BYTE*)(address + i)) = value;
			i += 1;

			/* removes 'value'; keeps 'key' for next iteration */
			lua_pop(L, 1);
		}
		//lua_next pops the key from the stack, if we reached the end, there is no key on the stack.

		lua_pop(L, 1); //removes 'table'

		// TODO: more popping?

		return 0;
	}

	int convertTableToByteStream(lua_State* L, std::stringstream* s) {
		int i = 0;

		//lua_pushvalue(L, 2); // push the table again; so that it is at -1

		/* table is in the stack at index 't' */
		lua_pushnil(L);  /* first key */
		while (lua_next(L, -2) != 0) {
			/* uses 'key' (at index -2) and 'value' (at index -1) */
			if (!lua_isinteger(L, -1)) {
				//lua_pushliteral(L, "The return value table must have integer values");
				//lua_error(L);
				return -1;
			}

			unsigned int value = lua_tointeger(L, -1);

			if (value <= 0xff && value >= 0x00) {
				s->write(reinterpret_cast<const char*>(&value), 1);
			}
			else {
				s->write(reinterpret_cast<const char*>(&value), 4);
			}


			i += 1;

			/* removes 'value'; keeps 'key' for next iteration */
			lua_pop(L, 1);
		}


		return 0;
	}

	int luaWriteCode(lua_State* L) {
		if (lua_gettop(L) != 2) {
			return luaL_error(L, "expected exactly 2 arguments");
		}
		DWORD address = lua_tointeger(L, 1);
		if (!lua_istable(L, 2)) {
			return luaL_error(L, "the second argument should be a table");
		}

		// write an intermediate state here that extracts the table and converts bytes to bytes
// and that converts integers to 4 bytes in big endian order.
// 
		lua_pushvalue(L, 2); // push the table so we can be sure it is at -1
		std::stringstream bytes;
		int returnCode = convertTableToByteStream(L, &bytes);
		lua_pop(L, 1); // pop the table;

		if (returnCode == -1) {
			return luaL_error(L, "The return value table must have integer values");
		}
		else if (returnCode == -2) {
			return luaL_error(L, "The values must all be positive");
		}

		bytes.seekg(0, bytes.end);
		int size = bytes.tellg();
		bytes.seekg(0, bytes.beg);

		DWORD oldProtect;
		VirtualProtect((LPVOID)address, size, PAGE_EXECUTE_READWRITE, &oldProtect);

		memcpy((void*)address, &bytes.str().data()[0], size);

		VirtualProtect((LPVOID)address, size, oldProtect, &oldProtect);

		return 0;
	}

	int luaMemCpy(lua_State* L) {

		if (lua_gettop(L) != 3) {
			return luaL_error(L, "expected exactly 3 arguments");
		}

		DWORD dst = lua_tointeger(L, 1);
		DWORD src = lua_tointeger(L, 2);
		int size = lua_tointeger(L, 3);

		memcpy((void*)dst, (void*)src, size);

		return 0;
	}


	std::set<std::string> stringSet;

	int registerString(lua_State* L) {
		if (lua_gettop(L) != 1) {
			return luaL_error(L, "Wrong number of arguments passed");
		}

		std::string target = lua_tostring(L, 1);

		std::pair<std::set<std::string>::iterator, bool> p = stringSet.insert(target);

		std::set<std::string>::iterator it = p.first;
		lua_pushinteger(L, (DWORD)p.first->c_str());

		return 1;
	}

	int luaAllocateRWE(lua_State* L) {
		if (lua_gettop(L) != 1) {
			return luaL_error(L, "Wrong number of arguments passed");
		}

		int size = lua_tonumber(L, 1);

		SYSTEM_INFO system_info;
		GetSystemInfo(&system_info);
		auto const page_size = system_info.dwPageSize;

		LPVOID adr = HeapAlloc(heap, 0, size);
		if (adr == 0) {
			return luaL_error(L, "failed to allocate executable memory");
		}

		lua_pushinteger(L, (DWORD_PTR)adr);

		return 1;
	}

	int luaAllocate(lua_State* L) {
		if (lua_gettop(L) != 1) {
			return luaL_error(L, "Wrong number of arguments passed");
		}

		int size = lua_tonumber(L, 1);

		void* memory = malloc(size);

		lua_pushinteger(L, (DWORD_PTR)memory);

		return 1;
	}

	int luaScanForAOB(lua_State* L) {
		DWORD min = 0;
		DWORD max = 0x7FFFFFFF;

		if (lua_gettop(L) == 1) {

		}
		else if (lua_gettop(L) == 2) {
			min = lua_tointeger(L, 2);
		}
		else if (lua_gettop(L) == 3) {
			min = lua_tointeger(L, 2);
			max = lua_tointeger(L, 3);
		}
		else {
			return luaL_error(L, "Expected exactly one, two, or three arguments");
		}

		std::string query = lua_tostring(L, 1);

		DWORD address = AOB::FindInRange(query, min, max);

		lua_pushinteger(L, address);

		return 1;
	}

	std::string luaEntryPointFile;

	void setEntryPoint(std::string filePath) {
		luaEntryPointFile = filePath;
	}

	std::string luaPackagePath;

	void setPackagePath(std::string packagePath) {
		luaPackagePath = packagePath;
	}

	void initialize() {
		heap = HeapCreate(HEAP_CREATE_ENABLE_EXECUTE, 0, 0); // start out with one page

		luaEntryPointFile;

		L = luaL_newstate();
		luaL_openlibs(L);

		lua_getglobal(L, "package");
		lua_pushstring(L, "path");
		lua_pushstring(L, luaPackagePath.c_str());
		lua_settable(L, -3);
		lua_pop(L, 1);

		lua_getglobal(L, "_G");
		luaL_setfuncs(L, printlib, 0);
		lua_pop(L, 1);

		lua_register(L, "hookCode", luaHookCode);
		lua_register(L, "callOriginal", cppCallCode);
		lua_register(L, "exposeCode", luaExposeCode);
		lua_register(L, "detourCode", luaDetourCode);
		lua_register(L, "allocate", luaAllocate);
		lua_register(L, "allocateCode", luaAllocateRWE);

		lua_register(L, "readByte", luaReadByte);
		lua_register(L, "readSmallInteger", luaReadSmallInteger);
		lua_register(L, "readInteger", luaReadInteger);
		lua_register(L, "readString", luaReadString);
		lua_register(L, "readBytes", luaReadBytes);

		lua_register(L, "writeByte", luaWriteByte);
		lua_register(L, "writeSmallInteger", luaWriteSmallInteger);
		lua_register(L, "writeInteger", luaWriteInteger);
		lua_register(L, "writeBytes", luaWriteBytes);
		lua_register(L, "writeCode", luaWriteCode);

		lua_register(L, "copyMemory", luaMemCpy);

		lua_register(L, "registerString", registerString);

		lua_register(L, "scanForAOB", luaScanForAOB);

		int r = luaL_dofile(L, luaEntryPointFile.c_str());

		if (r == LUA_OK) {
			std::cout << "[LUA]: loaded LUA API." << std::endl;
		}
		else {
			std::string errormsg = lua_tostring(L, -1);
			std::cout << "[LUA]: failed to load LUA API: " << errormsg << std::endl;
			lua_pop(L, 1); // pop off the error message;
		}
	}

	void deinitialize() {
		lua_close(L);

		if (heap != 0) {
			HeapDestroy(heap);
		}
	}

	void executeSnippet(std::string code) {
		int before = lua_gettop(L);
		int r = luaL_dostring(L, code.c_str());
		if (r == LUA_OK) {
			int after = lua_gettop(L);
			for (int i = before; i < after; i++) {
				int index = before - (i + 1);
				if (lua_isstring(L, index)) {
					std::cout << lua_tostring(L, index) << std::endl;
				}
				else if (lua_isnumber(L, index)) {
					std::cout << lua_tonumber(L, index) << std::endl;
				}
				else if (lua_isboolean(L, index)) {
					std::cout << lua_toboolean(L, index) << std::endl;
				}
				else if (lua_isnil(L, index)) {
					std::cout << "nil" << std::endl;
				}
				else {
					std::cout << "[object]" << std::endl;
				}
			}
			lua_pop(L, after - before);

		}
		else {
			std::string errormsg = lua_tostring(L, -1);
			std::cout << "[LUA]: " << errormsg << std::endl;
			lua_pop(L, 1); // pop off the error message;
		}
	}

	int getCurrentStackSize() {
		return lua_gettop(L);
	}

	lua_State* getLuaState() {
		return L;
	}


}