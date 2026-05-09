/**
 * @file MEGAListAllNodesFilter.h
 * @brief Filter for MEGASdk listAllNodesByPage queries.
 *
 * (c) 2026- by Mega Limited, Auckland, New Zealand
 *
 * This file is part of the MEGA SDK - Client Access Engine.
 *
 * Applications using the MEGA API must present a valid application key
 * and comply with the the rules set forth in the Terms of Service.
 *
 * The MEGA SDK is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 *
 * @copyright Simplified (2-clause) BSD License.
 *
 * You should have received a copy of the license along with this
 * program.
 */
#import <Foundation/Foundation.h>
#import "MEGASearchFilter.h"

typedef NS_ENUM (NSInteger, MEGAListAllNodesFilterSensitivityOption) {
    MEGAListAllNodesFilterSensitivityOptionDisabled = 0,
    MEGAListAllNodesFilterSensitivityOptionExcludeSensitive = 1
};

typedef NS_ENUM (NSInteger, MEGAListAllNodesFilterLocation) {
    MEGAListAllNodesFilterLocationCloudDrive = 0,
    MEGAListAllNodesFilterLocationCloudDriveAndVault = 1,
    MEGAListAllNodesFilterLocationCloudDriveVaultAndRubbish = 2
};

/**
 * @brief Maximum number of handles accepted by `locationHandles` and
 * `excludeLocationHandles`. Lists exceeding this size cause
 * `listAllNodesByPage` to reject the request and return an empty list.
 */
extern const NSUInteger MEGAListAllNodesFilterMaxLocationHandles;

NS_ASSUME_NONNULL_BEGIN

/**
 * @brief Filter for MEGASdk listAllNodesByPage.
 *
 * Deliberately narrower than MEGASearchFilter: only the fields the flat,
 * cursor-paginated listAllNodesByPage query can honour are exposed.
 *
 * Scope (resolved in priority order):
 *   - locationHandles set (non-empty, max 3) → results are the union of the
 *     descendant subtrees of the supplied ancestor handles. If a handle
 *     points into Rubbish or an in-share, those descendants are returned.
 *   - locationHandles unset / empty → location selects the rootnodes:
 *       MEGAListAllNodesFilterLocationCloudDrive                    →
 *           Cloud Drive only (i.e. excludes My Backups).
 *       MEGAListAllNodesFilterLocationCloudDriveAndVault (default)  →
 *           Cloud Drive + Vault (My Backups). Rubbish and in-shares
 *           always excluded in this and the previous mode.
 *       MEGAListAllNodesFilterLocationCloudDriveVaultAndRubbish     →
 *           Cloud Drive + Vault + Rubbish.
 *
 * Exclude list:
 *   - excludeLocationHandles (max 3) is applied on top of the scope above.
 *     Any node whose ancestor chain — including the node itself or the
 *     matched root — contains any of these handles is dropped. Setting the
 *     Vault root in this list therefore drops the entire Vault subtree.
 *
 * File versions are always excluded regardless of scope.
 */
@interface MEGAListAllNodesFilter : NSObject

/**
 * @brief Required. MIME type category (see MEGANodeFormatType in MEGASearchFilter.h).
 *
 * Must be a non-default value. listAllNodesByPage rejects
 * MEGANodeFormatTypeUnknown (returns an empty list and logs a warning).
 */
@property (nonatomic) MEGANodeFormatType category;

/**
 * @brief Optional. Restrict results to descendants of one or more ancestor
 * handles (max `MEGAListAllNodesFilterMaxLocationHandles`, currently 3).
 *
 * Leave nil or empty (default) to use the rootnode scope chosen by
 * `location`. Lists with more than the maximum number of entries, or with an
 * INVALID_HANDLE entry, cause listAllNodesByPage to reject the request.
 */
@property (nonatomic, copy, nullable) NSArray<NSNumber *> *locationHandles;

/**
 * @brief Optional. Drop nodes whose ancestor chain contains any of the
 * supplied handles (max `MEGAListAllNodesFilterMaxLocationHandles`).
 *
 * "Ancestor chain" is inclusive of the node itself and of the matched root,
 * so passing a root handle here drops its entire subtree.
 *
 * Applied independently of `locationHandles` / `location`. Pass nil or an
 * empty array (default) to disable.
 */
@property (nonatomic, copy, nullable) NSArray<NSNumber *> *excludeLocationHandles;

/**
 * @brief Optional. Rootnode scope selector. Ignored when `locationHandles`
 * is set (non-empty).
 *
 * Default: MEGAListAllNodesFilterLocationCloudDriveAndVault.
 */
@property (nonatomic) MEGAListAllNodesFilterLocation location;

/**
 * @brief Optional. Sensitivity filter.
 *
 * - MEGAListAllNodesFilterSensitivityOptionDisabled (default) — no filtering.
 * - MEGAListAllNodesFilterSensitivityOptionExcludeSensitive — hide nodes whose
 *   own SENSITIVE flag, or any strict ancestor's flag below the matched
 *   root, is set.
 */
@property (nonatomic) MEGAListAllNodesFilterSensitivityOption sensitivityFilter;

- (instancetype)init;

@end

NS_ASSUME_NONNULL_END
