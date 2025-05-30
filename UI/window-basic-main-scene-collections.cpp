/******************************************************************************
    Copyright (C) 2023 by Lain Bailey <lain@obsproject.com>

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 2 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program.  If not, see <http://www.gnu.org/licenses/>.
******************************************************************************/

#include <filesystem>
#include <string>

#include <obs.hpp>
#include <util/util.hpp>
#include <QMessageBox>
#include <QVariant>
#include <QFileDialog>
#include <QStandardPaths>
#include <qt-wrappers.hpp>
#include "item-widget-helpers.hpp"
#include "window-basic-main.hpp"
#include "window-importer.hpp"
#include "window-namedialog.hpp"

// MARK: Constant Expressions

constexpr std::string_view OBSSceneCollectionPath = "/obs-studio/basic/scenes/";

// MARK: - Anonymous Namespace
namespace {
QList<QString> sortedSceneCollections{};

void updateSortedSceneCollections(const OBSSceneCollectionCache &collections)
{
	const QLocale locale = QLocale::system();
	QList<QString> newList{};

	for (auto [collectionName, _] : collections) {
		QString entry = QString::fromStdString(collectionName);
		newList.append(entry);
	}

	std::sort(newList.begin(), newList.end(), [&locale](const QString &lhs, const QString &rhs) -> bool {
		int result = QString::localeAwareCompare(locale.toLower(lhs), locale.toLower(rhs));

		return (result < 0);
	});

	sortedSceneCollections.swap(newList);
}

void cleanBackupCollision(const OBSSceneCollection &collection)
{
	std::filesystem::path backupFilePath = collection.collectionFile;
	backupFilePath.replace_extension(".json.bak");

	if (std::filesystem::exists(backupFilePath)) {
		try {
			std::filesystem::remove(backupFilePath);
		} catch (std::filesystem::filesystem_error &) {
			throw std::logic_error("Failed to remove pre-existing scene collection backup file: " +
					       backupFilePath.u8string());
		}
	}
}
} // namespace

// MARK: - Main Scene Collection Management Functions

void OBSBasic::SetupNewSceneCollection(const std::string &collectionName)
{
	if (collectionName.empty()) {
		throw std::logic_error("Cannot create new scene collection with empty collection name");
	}

	const OBSSceneCollection &newCollection = CreateSceneCollection(collectionName);

	OnEvent(OBS_FRONTEND_EVENT_SCENE_COLLECTION_CHANGING);

	cleanBackupCollision(newCollection);
	ActivateSceneCollection(newCollection);

	blog(LOG_INFO, "Created scene collection '%s' (clean, %s)", newCollection.name.c_str(),
	     newCollection.fileName.c_str());
	blog(LOG_INFO, "------------------------------------------------");
}

void OBSBasic::SetupDuplicateSceneCollection(const std::string &collectionName)
{
	const OBSSceneCollection &newCollection = CreateSceneCollection(collectionName);
	const OBSSceneCollection &currentCollection = GetCurrentSceneCollection();

	SaveProjectNow();

	const auto copyOptions = std::filesystem::copy_options::overwrite_existing;

	try {
		std::filesystem::copy(currentCollection.collectionFile, newCollection.collectionFile, copyOptions);
	} catch (const std::filesystem::filesystem_error &error) {
		blog(LOG_DEBUG, "%s", error.what());
		throw std::logic_error("Failed to copy file for cloned scene collection: " + newCollection.name);
	}

	OBSDataAutoRelease collection = obs_data_create_from_json_file(newCollection.collectionFile.u8string().c_str());

	obs_data_set_string(collection, "name", newCollection.name.c_str());

	OBSDataArrayAutoRelease sources = obs_data_get_array(collection, "sources");

	if (sources) {
		obs_data_erase(collection, "sources");

		obs_data_array_enum(
			sources,
			[](obs_data_t *data, void *) -> void {
				const char *uuid = os_generate_uuid();

				obs_data_set_string(data, "uuid", uuid);

				bfree((void *)uuid);
			},
			nullptr);

		obs_data_set_array(collection, "sources", sources);
	}

	obs_data_save_json_safe(collection, newCollection.collectionFile.u8string().c_str(), "tmp", nullptr);

	cleanBackupCollision(newCollection);
	ActivateSceneCollection(newCollection);

	blog(LOG_INFO, "Created scene collection '%s' (duplicate, %s)", newCollection.name.c_str(),
	     newCollection.fileName.c_str());
	blog(LOG_INFO, "------------------------------------------------");
}

void OBSBasic::SetupRenameSceneCollection(const std::string &collectionName)
{
	const OBSSceneCollection &newCollection = CreateSceneCollection(collectionName);
	const OBSSceneCollection currentCollection = GetCurrentSceneCollection();

	SaveProjectNow();

	const auto copyOptions = std::filesystem::copy_options::overwrite_existing;

	try {
		std::filesystem::copy(currentCollection.collectionFile, newCollection.collectionFile, copyOptions);
	} catch (const std::filesystem::filesystem_error &error) {
		blog(LOG_DEBUG, "%s", error.what());
		throw std::logic_error("Failed to copy file for scene collection: " + currentCollection.name);
	}

	collections.erase(currentCollection.name);

	OBSDataAutoRelease collection = obs_data_create_from_json_file(newCollection.collectionFile.u8string().c_str());

	obs_data_set_string(collection, "name", newCollection.name.c_str());

	obs_data_save_json_safe(collection, newCollection.collectionFile.u8string().c_str(), "tmp", nullptr);

	cleanBackupCollision(newCollection);
	ActivateSceneCollection(newCollection);
	RemoveSceneCollection(currentCollection);

	blog(LOG_INFO, "Renamed scene collection '%s' to '%s' (%s)", currentCollection.name.c_str(),
	     newCollection.name.c_str(), newCollection.fileName.c_str());
	blog(LOG_INFO, "------------------------------------------------");

	OnEvent(OBS_FRONTEND_EVENT_SCENE_COLLECTION_RENAMED);
}

// MARK: - Scene Collection File Management Functions

const OBSSceneCollection &OBSBasic::CreateSceneCollection(const std::string &collectionName)
{
	if (const auto &foundCollection = GetSceneCollectionByName(collectionName)) {
		throw std::invalid_argument("Scene collection already exists: " + collectionName);
	}

	std::string fileName;
	if (!GetFileSafeName(collectionName.c_str(), fileName)) {
		throw std::invalid_argument("Failed to create safe directory for new scene collection: " +
					    collectionName);
	}

	std::string collectionFile;
	collectionFile.reserve(App()->userScenesLocation.u8string().size() + OBSSceneCollectionPath.size() +
			       fileName.size());
	collectionFile.append(App()->userScenesLocation.u8string()).append(OBSSceneCollectionPath).append(fileName);

	if (!GetClosestUnusedFileName(collectionFile, "json")) {
		throw std::invalid_argument("Failed to get closest file name for new scene collection: " + fileName);
	}

	const std::filesystem::path collectionFilePath = std::filesystem::u8path(collectionFile);

	auto [iterator, success] = collections.try_emplace(
		collectionName,
		OBSSceneCollection{collectionName, collectionFilePath.filename().u8string(), collectionFilePath});

	return iterator->second;
}

void OBSBasic::RemoveSceneCollection(OBSSceneCollection collection)
{
	try {
		std::filesystem::remove(collection.collectionFile);
	} catch (const std::filesystem::filesystem_error &error) {
		blog(LOG_DEBUG, "%s", error.what());
		throw std::logic_error("Failed to remove scene collection file: " + collection.fileName);
	}

	blog(LOG_INFO, "Removed scene collection '%s' (%s)", collection.name.c_str(), collection.fileName.c_str());
	blog(LOG_INFO, "------------------------------------------------");
}

// MARK: - Scene Collection UI Handling Functions

bool OBSBasic::CreateNewSceneCollection(const QString &name)
{
	try {
		SetupNewSceneCollection(name.toStdString());
		return true;
	} catch (const std::invalid_argument &error) {
		blog(LOG_ERROR, "%s", error.what());
		return false;
	} catch (const std::logic_error &error) {
		blog(LOG_ERROR, "%s", error.what());
		return false;
	}
}

bool OBSBasic::CreateDuplicateSceneCollection(const QString &name)
{
	try {
		SetupDuplicateSceneCollection(name.toStdString());
		return true;
	} catch (const std::invalid_argument &error) {
		blog(LOG_ERROR, "%s", error.what());
		return false;
	} catch (const std::logic_error &error) {
		blog(LOG_ERROR, "%s", error.what());
		return false;
	}
}

void OBSBasic::DeleteSceneCollection(const QString &name)
{
	const std::string_view currentCollectionName{
		config_get_string(App()->GetUserConfig(), "Basic", "SceneCollection")};

	if (currentCollectionName == name.toStdString()) {
		on_actionRemoveSceneCollection_triggered();
		return;
	}

	OBSSceneCollection currentCollection = GetCurrentSceneCollection();

	RemoveSceneCollection(currentCollection);

	collections.erase(name.toStdString());

	RefreshSceneCollections();

	OnEvent(OBS_FRONTEND_EVENT_SCENE_COLLECTION_LIST_CHANGED);
}

void OBSBasic::ChangeSceneCollection()
{
	QAction *action = reinterpret_cast<QAction *>(sender());

	if (!action) {
		return;
	}

	const std::string_view currentCollectionName{
		config_get_string(App()->GetUserConfig(), "Basic", "SceneCollection")};
	const QVariant qCollectionName = action->property("collection_name");
	const std::string selectedCollectionName{qCollectionName.toString().toStdString()};

	if (currentCollectionName == selectedCollectionName) {
		action->setChecked(true);
		return;
	}

	const std::optional<OBSSceneCollection> foundCollection = GetSceneCollectionByName(selectedCollectionName);

	if (!foundCollection) {
		const std::string errorMessage{"Selected scene collection not found: "};

		throw std::invalid_argument(errorMessage + currentCollectionName.data());
	}

	const OBSSceneCollection &selectedCollection = foundCollection.value();

	OnEvent(OBS_FRONTEND_EVENT_SCENE_COLLECTION_CHANGING);

	ActivateSceneCollection(selectedCollection);

	blog(LOG_INFO, "Switched to scene collection '%s' (%s)", selectedCollection.name.c_str(),
	     selectedCollection.fileName.c_str());
	blog(LOG_INFO, "------------------------------------------------");
}

void OBSBasic::RefreshSceneCollections(bool refreshCache)
{
	std::string_view currentCollectionName{config_get_string(App()->GetUserConfig(), "Basic", "SceneCollection")};

	QList<QAction *> menuActions = ui->sceneCollectionMenu->actions();

	for (auto &action : menuActions) {
		QVariant variant = action->property("file_name");
		if (variant.typeName() != nullptr) {
			delete action;
		}
	}

	if (refreshCache) {
		RefreshSceneCollectionCache();
	}

	updateSortedSceneCollections(collections);

	size_t numAddedCollections = 0;
	for (auto &name : sortedSceneCollections) {
		const std::string collectionName = name.toStdString();
		try {
			const OBSSceneCollection &collection = collections.at(collectionName);
			const QString qCollectionName = QString().fromStdString(collectionName);

			QAction *action = new QAction(qCollectionName, this);
			action->setProperty("collection_name", qCollectionName);
			action->setProperty("file_name", QString().fromStdString(collection.fileName));
			connect(action, &QAction::triggered, this, &OBSBasic::ChangeSceneCollection);
			action->setCheckable(true);
			action->setChecked(collectionName == currentCollectionName);

			ui->sceneCollectionMenu->addAction(action);

			numAddedCollections += 1;
		} catch (const std::out_of_range &error) {
			blog(LOG_ERROR, "No scene collection with name %s found in scene collection cache.\n%s",
			     collectionName.c_str(), error.what());
		}
	}

	ui->actionRemoveSceneCollection->setEnabled(numAddedCollections > 1);

	OBSBasic *main = reinterpret_cast<OBSBasic *>(App()->GetMainWindow());

	main->ui->actionPasteFilters->setEnabled(false);
	main->ui->actionPasteRef->setEnabled(false);
	main->ui->actionPasteDup->setEnabled(false);
}

// MARK: - Scene Collection Cache Functions

void OBSBasic::RefreshSceneCollectionCache()
{
	OBSSceneCollectionCache foundCollections{};

	const std::filesystem::path collectionsPath =
		App()->userScenesLocation / std::filesystem::u8path(OBSSceneCollectionPath.substr(1));

	if (!std::filesystem::exists(collectionsPath)) {
		blog(LOG_WARNING, "Failed to get scene collections config path");
		return;
	}

	for (const auto &entry : std::filesystem::directory_iterator(collectionsPath)) {
		if (entry.is_directory()) {
			continue;
		}

		if (entry.path().extension().u8string() != ".json") {
			continue;
		}

		OBSDataAutoRelease collectionData =
			obs_data_create_from_json_file_safe(entry.path().u8string().c_str(), "bak");

		std::string candidateName;
		std::string collectionName = obs_data_get_string(collectionData, "name");

		if (collectionName.empty()) {
			candidateName = entry.path().stem().u8string();
		} else {
			candidateName = std::move(collectionName);
		}

		foundCollections.try_emplace(candidateName,
					     OBSSceneCollection{candidateName, entry.path().filename().u8string(),
								entry.path()});
	}

	collections.swap(foundCollections);
}

const OBSSceneCollection &OBSBasic::GetCurrentSceneCollection() const
{
	std::string currentCollectionName{config_get_string(App()->GetUserConfig(), "Basic", "SceneCollection")};

	if (currentCollectionName.empty()) {
		throw std::invalid_argument("No valid scene collection name in configuration Basic->SceneCollection");
	}

	const auto &foundCollection = collections.find(currentCollectionName);

	if (foundCollection != collections.end()) {
		return foundCollection->second;
	} else {
		throw std::invalid_argument("Scene collection not found in collection list: " + currentCollectionName);
	}
}

std::optional<OBSSceneCollection> OBSBasic::GetSceneCollectionByName(const std::string &collectionName) const
{
	auto foundCollection = collections.find(collectionName);

	if (foundCollection == collections.end()) {
		return {};
	} else {
		return foundCollection->second;
	}
}

std::optional<OBSSceneCollection> OBSBasic::GetSceneCollectionByFileName(const std::string &fileName) const
{
	for (auto &[iterator, collection] : collections) {
		if (collection.fileName == fileName) {
			return collection;
		}
	}

	return {};
}

// MARK: - Qt Slot Functions

void OBSBasic::on_actionNewSceneCollection_triggered()
{
	const OBSPromptCallback sceneCollectionCallback = [this](const OBSPromptResult &result) {
		if (GetSceneCollectionByName(result.promptValue)) {
			return false;
		}

		return true;
	};

	const OBSPromptRequest request{Str("Basic.Main.AddSceneCollection.Title"),
				       Str("Basic.Main.AddSceneCollection.Text")};

	OBSPromptResult result = PromptForName(request, sceneCollectionCallback);

	if (!result.success) {
		return;
	}

	try {
		SetupNewSceneCollection(result.promptValue);
	} catch (const std::invalid_argument &error) {
		blog(LOG_ERROR, "%s", error.what());
	} catch (const std::logic_error &error) {
		blog(LOG_ERROR, "%s", error.what());
	}
}

void OBSBasic::on_actionDupSceneCollection_triggered()
{
	const OBSPromptCallback sceneCollectionCallback = [this](const OBSPromptResult &result) {
		if (GetSceneCollectionByName(result.promptValue)) {
			return false;
		}

		return true;
	};

	const OBSPromptRequest request{Str("Basic.Main.AddSceneCollection.Title"),
				       Str("Basic.Main.AddSceneCollection.Text")};

	OBSPromptResult result = PromptForName(request, sceneCollectionCallback);

	if (!result.success) {
		return;
	}

	try {
		SetupDuplicateSceneCollection(result.promptValue);
	} catch (const std::invalid_argument &error) {
		blog(LOG_ERROR, "%s", error.what());
	} catch (const std::logic_error &error) {
		blog(LOG_ERROR, "%s", error.what());
	}
}

void OBSBasic::on_actionRenameSceneCollection_triggered()
{
	const OBSSceneCollection &currentCollection = GetCurrentSceneCollection();

	const OBSPromptCallback sceneCollectionCallback = [this](const OBSPromptResult &result) {
		if (GetSceneCollectionByName(result.promptValue)) {
			return false;
		}

		return true;
	};

	const OBSPromptRequest request{Str("Basic.Main.RenameSceneCollection.Title"),
				       Str("Basic.Main.AddSceneCollection.Text"), currentCollection.name};

	OBSPromptResult result = PromptForName(request, sceneCollectionCallback);

	if (!result.success) {
		return;
	}

	try {
		SetupRenameSceneCollection(result.promptValue);
	} catch (const std::invalid_argument &error) {
		blog(LOG_ERROR, "%s", error.what());
	} catch (const std::logic_error &error) {
		blog(LOG_ERROR, "%s", error.what());
	}
}

void OBSBasic::on_actionRemoveSceneCollection_triggered(bool skipConfirmation)
{
	if (collections.size() < 2) {
		return;
	}

	OBSSceneCollection currentCollection;

	try {
		currentCollection = GetCurrentSceneCollection();

		if (!skipConfirmation) {
			const QString confirmationText =
				QTStr("ConfirmRemove.Text").arg(QString::fromStdString(currentCollection.name));
			const QMessageBox::StandardButton button =
				OBSMessageBox::question(this, QTStr("ConfirmRemove.Title"), confirmationText);

			if (button == QMessageBox::No) {
				return;
			}
		}

		OnEvent(OBS_FRONTEND_EVENT_SCENE_COLLECTION_CHANGING);

		collections.erase(currentCollection.name);
	} catch (const std::invalid_argument &error) {
		blog(LOG_ERROR, "%s", error.what());
	} catch (const std::logic_error &error) {
		blog(LOG_ERROR, "%s", error.what());
	}

	const OBSSceneCollection &newCollection = collections.begin()->second;

	ActivateSceneCollection(newCollection);
	RemoveSceneCollection(currentCollection);

	blog(LOG_INFO, "Switched to scene collection '%s' (%s)", newCollection.name.c_str(),
	     newCollection.fileName.c_str());
	blog(LOG_INFO, "------------------------------------------------");
}

void OBSBasic::on_actionImportSceneCollection_triggered()
{
	OBSImporter imp(this);
	imp.exec();

	RefreshSceneCollections(true);
}

void OBSBasic::on_actionExportSceneCollection_triggered()
{
	SaveProjectNow();

	const OBSSceneCollection &currentCollection = GetCurrentSceneCollection();

	const QString home = QDir::homePath();

	const QString destinationFileName = SaveFile(this, QTStr("Basic.MainMenu.SceneCollection.Export"),
						     home + "/" + currentCollection.fileName.c_str(),
						     "JSON Files (*.json)");

	if (!destinationFileName.isEmpty() && !destinationFileName.isNull()) {
		const std::filesystem::path sourceFile = currentCollection.collectionFile;
		const std::filesystem::path destinationFile =
			std::filesystem::u8path(destinationFileName.toStdString());

		OBSDataAutoRelease collection = obs_data_create_from_json_file(sourceFile.u8string().c_str());

		OBSDataArrayAutoRelease sources = obs_data_get_array(collection, "sources");
		if (!sources) {
			blog(LOG_WARNING, "No sources in exported scene collection");
			return;
		}

		obs_data_erase(collection, "sources");

		using OBSDataVector = std::vector<OBSData>;

		OBSDataVector sourceItems;
		obs_data_array_enum(
			sources,
			[](obs_data_t *data, void *vector) -> void {
				OBSDataVector &sourceItems{*static_cast<OBSDataVector *>(vector)};
				sourceItems.push_back(data);
			},
			&sourceItems);

		std::sort(sourceItems.begin(), sourceItems.end(), [](const OBSData &a, const OBSData &b) {
			return astrcmpi(obs_data_get_string(a, "name"), obs_data_get_string(b, "name")) < 0;
		});

		OBSDataArrayAutoRelease newSources = obs_data_array_create();
		for (auto &item : sourceItems) {
			obs_data_array_push_back(newSources, item);
		}

		obs_data_set_array(collection, "sources", newSources);
		obs_data_save_json_pretty_safe(collection, destinationFile.u8string().c_str(), "tmp", "bak");
	}
}

void OBSBasic::on_actionRemigrateSceneCollection_triggered()
{
	if (Active()) {
		OBSMessageBox::warning(this, QTStr("Basic.Main.RemigrateSceneCollection.Title"),
				       QTStr("Basic.Main.RemigrateSceneCollection.CannotMigrate.Active"));
		return;
	}

	if (!usingAbsoluteCoordinates && !migrationBaseResolution) {
		OBSMessageBox::warning(
			this, QTStr("Basic.Main.RemigrateSceneCollection.Title"),
			QTStr("Basic.Main.RemigrateSceneCollection.CannotMigrate.UnknownBaseResolution"));
		return;
	}

	obs_video_info ovi;
	obs_get_video_info(&ovi);

	if (!usingAbsoluteCoordinates && migrationBaseResolution->first == ovi.base_width &&
	    migrationBaseResolution->second == ovi.base_height) {
		OBSMessageBox::warning(
			this, QTStr("Basic.Main.RemigrateSceneCollection.Title"),
			QTStr("Basic.Main.RemigrateSceneCollection.CannotMigrate.BaseResolutionMatches"));
		return;
	}

	const OBSSceneCollection &currentCollection = GetCurrentSceneCollection();

	QString name = QString::fromStdString(currentCollection.name);
	QString message =
		QTStr("Basic.Main.RemigrateSceneCollection.Text").arg(name).arg(ovi.base_width).arg(ovi.base_height);

	auto answer = OBSMessageBox::question(this, QTStr("Basic.Main.RemigrateSceneCollection.Title"), message);

	if (answer == QMessageBox::No)
		return;

	lastOutputResolution = {ovi.base_width, ovi.base_height};

	if (!usingAbsoluteCoordinates) {
		/* Temporarily change resolution to migration resolution */
		ovi.base_width = migrationBaseResolution->first;
		ovi.base_height = migrationBaseResolution->second;

		if (obs_reset_video(&ovi) != OBS_VIDEO_SUCCESS) {
			OBSMessageBox::critical(
				this, QTStr("Basic.Main.RemigrateSceneCollection.Title"),
				QTStr("Basic.Main.RemigrateSceneCollection.CannotMigrate.FailedVideoReset"));
			return;
		}
	}

	OnEvent(OBS_FRONTEND_EVENT_SCENE_COLLECTION_CHANGING);

	/* Save and immediately reload to (re-)run migrations. */
	SaveProjectNow();
	/* Reset video if we potentially changed to a temporary resolution */
	if (!usingAbsoluteCoordinates) {
		ResetVideo();
	}

	ActivateSceneCollection(currentCollection, !usingAbsoluteCoordinates);
}

// MARK: - Scene Collection Management Helper Functions

void OBSBasic::ActivateSceneCollection(const OBSSceneCollection &collection, bool remigrate)
{
	const std::string currentCollectionName{config_get_string(App()->GetUserConfig(), "Basic", "SceneCollection")};

	if (auto foundCollection = GetSceneCollectionByName(currentCollectionName)) {
		if (collection.name != foundCollection.value().name) {
			SaveProjectNow();
		}
	}

	config_set_string(App()->GetUserConfig(), "Basic", "SceneCollection", collection.name.c_str());
	config_set_string(App()->GetUserConfig(), "Basic", "SceneCollectionFile", collection.fileName.c_str());

	Load(collection.collectionFile.u8string().c_str(), remigrate);

	RefreshSceneCollections();

	UpdateTitleBar();

	OnEvent(OBS_FRONTEND_EVENT_SCENE_COLLECTION_LIST_CHANGED);
	OnEvent(OBS_FRONTEND_EVENT_SCENE_COLLECTION_CHANGED);
}
