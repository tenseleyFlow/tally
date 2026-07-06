class Tally < Formula
  desc "GNU wc reimplementation: byte-identical output, SIMD word counting"
  homepage "https://github.com/tenseleyFlow/tally"
  url "https://github.com/tenseleyFlow/tally/releases/download/v0.1.0/tally-0.1.0.tar.gz"
  sha256 "8ed3e585500bd0091fcb32beae097d84a3d1b304de46e5b51796d1b976c6358f"
  license "GPL-3.0-or-later"
  head "https://github.com/tenseleyFlow/tally.git", branch: "trunk"

  def install
    system "./configure"
    system "make", "release"
    system "make", "install", "PREFIX=#{prefix}"
  end

  test do
    assert_match "tally #{version}", shell_output("#{bin}/tally --version")
    assert_equal %w[0 0 0], shell_output("printf '' | #{bin}/tally").split
    # ty is the same binary under a shorter name.
    assert_equal "3", shell_output("printf 'a b c' | #{bin}/ty -w").strip
  end
end
