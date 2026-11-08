package com.pockethle.app

/** Installed Samsung bada package metadata for the launcher. */
data class GameEntry(
    val id: String,
    val displayName: String,
    val executable: String,
    val sourcePackage: String,
    val importedAt: Long,
    val icon: String?,
    val directory: String,
    val appId: String,
    val lastPlayed: Long,
)
