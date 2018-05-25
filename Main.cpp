/*
 * Copyright (c) 2018 Guo Xiang
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, version 3.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program. If not, see <http://www.gnu.org/licenses/>.
 */

#include <EasyCpp.hpp>
#include <CmdlineParser.hpp>
#include <Io.hpp>
#include <Command.hpp>

//#define DEBUG_PM

#ifdef DEBUG_PM
#define PM_DEBUG(...) TRACE_INFO(__VA_ARGS__)
#else
#define PM_DEBUG(...)
#endif

struct PackFixInfo {
	IFolderPtr root;
	IFolderPtr package;
	CJsonPtr packs;
	bool forceUpdate;
};

struct DepInfo {
	CStringList depList;
	CStringList sLibList;
	CStringList dLibList;
};

struct PackInfo {
	PackInfo(PackFixInfo &_fix,
			 DepInfo &_dep,
			 const CConstStringPtr &_name,
			 const CConstStringPtr &_version) :
		fix(_fix),
		dep(_dep),
		name(_name),
		version(_version),
		details(nullptr),
		path(nullptr),
		result(false)
   	{}

	PackFixInfo &fix;
	DepInfo &dep;
	const CConstStringPtr name;
	const CConstStringPtr version;
	CJsonPtr details;
	IFolderPtr path;
	bool result;
};

static void AddPackage(PackInfo &info);

static void FetchCodes(const IDirentPtr &path,
					   const CConstStringPtr &name,
					   const CConstStringPtr &url,
					   const CConstStringPtr &rev)
{
	CConstStringPtr cwd(Platform::GetDir());
	CConstStringPtr packPath(cwd, "/", path->GetPath());

	Platform::ChangeDir(packPath);
	Platform::System(CConstStringPtr("git clone ", url, " ", name));

	Platform::ChangeDir(CConstStringPtr(packPath, "/", name));
	Platform::System(CConstStringPtr("git reset --hard ", rev));

	Platform::ChangeDir(cwd);
}

static void CheckDependency(PackInfo &info, DepInfo &dInfo)
{
	/* Check dependency */
	info.details->GetChildByKey("Dependency")->Then([&](const CJsonPtr &dep) {
		PM_DEBUG("Find dependencies\n");
		return dep->GetChildren()->ForEach();
	})->Then([&](const CJsonPtr &dep) {
		CConstStringPtr dName(dep->GetChildByKey("Name"));
		PackInfo _info(info.fix, dInfo, dName, dep->GetChildByKey("Version"));
		AddPackage(_info);

		PM_DEBUG("Find dependency: ", dName, " version: ",
				 dep->GetChildByKey("Version"), EOS);

		/* Update direct dependency list */
		dInfo.depList += dName;

		dep->GetChildByKey("LinkType")->Then([&](const CConstStringPtr &val) {
			val->Switch()
				->Case("Static", [&](void) {
					dInfo.sLibList += dName;
				})->Case("Dynamic", [&](void) {
					dInfo.dLibList += dName;
				});
		});
	});
}

DEFINE_SYNC_PROMISE(UpdatePack, IFilePtr);
static decltype(auto) UpdatePackage(PackInfo &info)
{
	CConstStringPtr jName(info.name, ".json");

	/* Not force update */
	if (!info.fix.forceUpdate) {
		bool exist = info.path->Find(jName)->Then([&](const IDirentPtr &file) {
			return (!file->IsFolder() &&
					file->ToFile()->Map()->ToJson()->GetChildByKey("Version") == info.version);
		})->Catch([&](void) {
			return false;
		});

		if (exist) {
			TRACE_INFO(info.name, " is already in version: ", info.version, EOS);
			return CUpdatePackPromisePtr();
		}
	}

	/* Write package info */
	info.path->CreateFile(jName)->Write(CJsonPtr()
										->AddChild("name", info.name)
										->AddChild("Version", info.version)
										->ToString());

	info.path->Delete(info.name);

	FetchCodes(info.path->ToDirent(),
			   info.name,
			   info.details->GetChildByKey("URL"),
			   info.details->GetChildByKey("Revision"));

	return CUpdatePackPromisePtr(info.path->CreateFile(CConstStringPtr(info.name, ".mk")));
}

static void AddFramework(PackInfo &info)
{
	/* The framework will be put in current folder */
	info.path = info.fix.root;

	CConstStringPtr jName(info.name, ".json");

	/* Not force update */
	if (!info.fix.forceUpdate) {
		bool exist = info.path->Find(jName)->Then([&](const IDirentPtr &file) {
			return (!file->IsFolder() &&
					file->ToFile()->Map()->ToJson()->GetChildByKey("Version") == info.version);
		})->Catch([&](void) {
			return false;
		});

		if (exist) {
			TRACE_INFO(info.name, " is already in version: ", info.version, EOS);
			return;
		}
	}

	info.path->Delete("EasyCpp");

	/* Write package info */
	info.path->CreateFile(jName)->Write(CJsonPtr()
										->AddChild("name", info.name)
										->AddChild("Version", info.version)
										->ToString());

	auto url(info.details->GetChildByKey("URL"));
	auto rev(info.details->GetChildByKey("Revision"));
	CConstStringPtr cwd(Platform::GetDir());

	Platform::System(CConstStringPtr("git clone ", url, " EasyCpp"));
	Platform::ChangeDir(CConstStringPtr(cwd, "/EasyCpp"));
	Platform::System(CConstStringPtr("git reset --hard ", rev));
	Platform::ChangeDir(cwd);
}

static void AddInterface(PackInfo &info)
{
	DepInfo dInfo;

	CheckDependency(info, dInfo);

	/* All interface will be put in Interface folder */
	info.path = info.fix.package->CreateFolder("Interface");

	UpdatePackage(info)->Then([&](const IFilePtr &file) {
		file->Write("export FLAGS += \\\n")
			->Write("  -I $(PACKAGES)/Interface/")->Write(info.name)->Write("\n\n");

		file->Write(".PHONY: ")->Write(info.name)->Write(EOS)
			->Write(info.name)->Write(": ");

		dInfo.depList.Iter()->ForEach([&](const CConstStringPtr &dep) {
			file->Write(" ")->Write(dep);
		});
	});
}

static void Add(PackInfo &info, bool isApp)
{
	DepInfo dInfo;

	CheckDependency(info, dInfo);

	/* Create json and mk file */
	auto &name(info.name);
	auto path = "Packages/";
	info.path = info.fix.package;

	info.details->GetChildren()->Find("Platform")->Then([&](const CConstStringPtr &val) {
		val->Switch()
			->Case("Linux", [&](void) {
				info.path = info.path->CreateFolder("Platform")->CreateFolder("Linux");
				path = "Packages/Platform/Linux/";
			});
	});

	UpdatePackage(info)->Then([&](const IFilePtr &file) {
		file->Write("export FLAGS += \\\n")
			->Write("  -I $(PACKAGES)/")->Write(info.name)->Write("/Inc\n\n");

		file->Write(".PHONY: ")->Write(name)->Write(EOS)
			->Write(name)->Write(":");

		dInfo.depList.Iter()->ForEach([&](const CConstStringPtr &dep) {
			file->Write(" ")->Write(dep);
		});

		file->Write(EOS)
			->Write("\t@$(MAKE) -f $(PACKAGES)/")->Write(name)->Write("/Makefile \\\n")
			->Write("\t\tPKG_PATH=")->Write(path)->Write(name)->Write(" \\\n")
			->Write("\t\tPKG_NAME=")->Write(name)->Write(" \\\n")
			->Write("\t\tSLIBS=\"");

		dInfo.sLibList.Iter()->ForEach([&](const CConstStringPtr &slib) {
			file->Write(slib)->Write(" ");
		});

		file->Write("\" \\\n")->Write("\t\tDLIBS=\"EasyCpp ");

		dInfo.dLibList.Iter()->ForEach([&](const CConstStringPtr &dlib) {
			file->Write(dlib)->Write(" ");
		});
		file->Write("\" \\\n");

		if (isApp) {
			file->Write("\t\tI_AM_APP=y \\\n");
		}

		file->Write("\t\tall\n");

	});

	info.dep.depList += dInfo.depList;
	info.dep.sLibList += dInfo.sLibList;
	info.dep.dLibList += dInfo.dLibList;
}

static void AddPackage(PackInfo &info)
{
	TRACE_INFO("Finding package: ", info.name, "(", info.version, ")\n");

	/* Find the package by name */
	info.fix.packs->GetChildren()->Find([&](const CJsonPtr &pack) {
		PM_DEBUG("Compare: ", info.name, " vs ",
				 pack->GetChildByKey("Name"), EOS);
		return (info.name == pack->GetChildByKey("Name"));

	})->Then([&](const CJsonPtr &pack) {
		PM_DEBUG("Found: ", info.name, EOS);
		/* Find the version in the found package */
		pack->GetChildByKey("Versions")->Then([&](const CJsonPtr &json) {
			return json->GetChildren()->Find([&](const CJsonPtr &details) {
				PM_DEBUG("Comparing: ", info.version, " vs ",
						 details->GetChildByKey("Version"), EOS);
				return info.version == details->GetChildByKey("Version");
			});
		})->Then([&](const CJsonPtr &details) {
			PM_DEBUG("Found: ", info.version, EOS);
			/* pack -> package information for given package.
			 * details -> detail information for given version. */
			info.details = details;
			info.result = true;

			return pack->GetChildByKey("Type");
		})->Then([&](const CJsonPtr &json) {
			PM_DEBUG("Type: ", json, EOS);
			json->GetVal()->Switch()
				->Case("Interface", [&](void) {
					TRACE_INFO("Install package(Interface): ", info.name, "(", info.version, ")\n");
					AddInterface(info);
				})->Case("Lib", [&](void) {
					TRACE_INFO("Install package(Library): ", info.name, "(", info.version, ")\n");
					Add(info, false);
				})->Case("Exec", [&](void) {
					TRACE_INFO("Install package(Executable): ", info.name, "(", info.version, ")\n");
					Add(info, true);
				})->Case("Framework", [&](void) {
					TRACE_INFO("Install package(Executable): ", info.name, "(", info.version, ")\n");
					AddFramework(info);
				});
		});
	});
}

static void ListPackages(const CJsonPtr &json)
{
	json->GetChildren()->ForEach()->Then([&](const CJsonPtr &package) {
		TRACE_INFO("+ ", package->GetChildByKey("Name"), EOS);
		TRACE_INFO("|    Type: ", package->GetChildByKey("Type"), EOS);
		TRACE_INFO("|  + Version:\n");

		return package->GetChildByKey("Versions");

	})->Then([&](const CJsonPtr &versions) {
		return versions->GetChildren()->ForEach();

	})->Then([&](const CJsonPtr &version) {
		TRACE_INFO("|  |  + ", version->GetChildByKey("Version"), EOS);
		return version->GetChildByKey("Dependency");

	})->Then([&](const CJsonPtr &child) {
		return child->GetChildren()->ForEach();

	})->Then([&](const CJsonPtr &dep) {
		TRACE_INFO("|  |  |   Depends on: ", dep->GetChildByKey("Name"),
				   "(", dep->GetChildByKey("Version"), ")\n");
	});
}

static void Start(CCmdlineParser &parser)
{
	IFilePtr config(nullptr);

	if (parser.IsKeySet('i')) {
		config = CreateFile(parser.GetKeyArg('i', 0));
	} else {
		Platform::System("git clone https://github.com/jackygx/PackageManager.git Configuration");
		config = CreateFile("./Configuration/Packages.json");
	}

	CJsonPtr json(config->Map()->ToJson()->GetChildByKey("Packages"));

	if (parser.IsKeySet('l')) {
		ListPackages(json);
		return;
	}

	/* Create the Packages folder */
	auto root(CreateDirent(parser.IsKeySet('p') ?
				parser.GetKeyArg('p', 0) : "./")->ToFolder());
	auto package(root->CreateFolder("Packages"));

	PackFixInfo fInfo = {
		.root = root,
		.package = package,
		.packs = json,
		.forceUpdate = parser.IsKeySet('f'),
	};

	if (parser.IsKeySet('a')) {
		parser.GetKeyArgs('a', [&](const CConstStringPtr &param) {
			param->Split("/")->First([&](const CConstStringPtr &name,
										 const CString::IteratorPtr &iter) {
				iter->Rest([&](const CConstStringPtr &ver) {
					DepInfo dInfo;
					PackInfo info(fInfo, dInfo, name, ver);
					AddPackage(info);
				});
			});
		});
	}
}

int main(int argc, char *argv[])
{
	/* To show the pretty call stack */
	InitSymbolPath(argv[0]);

	try {
		CCmdlineParser parser(argc, argv);
		parser.AddKey('i', "Input json file");
		parser.AddKey('p', "Path to install the packages");
		parser.AddKey('a', "Add specified packages. Format: name/version");
		parser.AddKey('f', "Force update the packages");
		parser.AddKey('l', "List all available packages");
		parser.AddKey('h', "Show this help message");
		parser.Parse();

		if (parser.IsKeySet('h')) {
			parser.PrintUsage(0);
		}

		Start(parser);
	} catch (const IException &e) {
		e.Show();
	}

	return 0;
}

